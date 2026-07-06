#ifndef __tunnel_hub_hpp__
#define __tunnel_hub_hpp__

// Registry of live gRPC-tunnel sessions (dial-out targets) plus correlation of
// requests the server forwards down a target's held-open Session stream.
//
// A target dials in, opens the Session bidi stream, and sends a Register frame
// with its target_id; that id is recorded here with the owning grpc_session +
// stream so the server can push TunnelRequest frames DOWN to it.
//
// Two correlation kinds, keyed by a monotonic request id echoed in the reply:
//   * unary   (Get/Set)   — one reply completes the operator's response.
//   * stream  (Subscribe) — many replies relayed to the operator's stream until
//                           it ends.
// Single-threaded (libevent loop) — no lock.

#include "grpc_session.hpp"
#include "tunnel/tunnel.pb.h"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class tunnel_hub {
public:
  using reply_fn = std::function<void(int status, const std::string &payload)>;
  using msg_fn = std::function<void(const std::string &payload)>;
  using end_fn = std::function<void(int status)>;

  struct session {
    grpc_session *grpc{nullptr};
    std::int32_t stream_id{0};
  };

  static tunnel_hub &instance() {
    static tunnel_hub hub;
    return hub;
  }

  // ---- target sessions ------------------------------------------------------
  void add(const std::string &target_id, grpc_session *g, std::int32_t sid) {
    m_sessions[target_id] = session{g, sid};
  }
  bool connected(const std::string &target_id) const {
    return m_sessions.find(target_id) != m_sessions.end();
  }
  std::size_t size() const { return m_sessions.size(); }

  // Drop all sessions owned by g (target connection ended) and fail any pending
  // requests routed to those targets so operators don't hang.
  void remove(grpc_session *g) {
    std::vector<std::string> gone;
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
      if (it->second.grpc == g) {
        gone.push_back(it->first);
        it = m_sessions.erase(it);
      } else {
        ++it;
      }
    }
    for (const auto &t : gone)
      fail_target(t);
  }

  // Push a TunnelRequest down the target's stream; false if not connected.
  bool send(const std::string &target_id, const tunnel::TunnelRequest &req) {
    auto it = m_sessions.find(target_id);
    if (it == m_sessions.end() || it->second.grpc == nullptr)
      return false;
    std::string pb;
    req.SerializeToString(&pb);
    it->second.grpc->stream_send(it->second.stream_id, pb);
    return true;
  }

  std::uint64_t next_id() { return ++m_next_id; }

  // ---- unary (Get/Set) correlation -----------------------------------------
  void add_pending(std::uint64_t id, const std::string &target, reply_fn cb) {
    m_pending[id] = pending_req{target, std::move(cb)};
  }
  void cancel_pending(std::uint64_t id) { m_pending.erase(id); }

  // ---- streaming (Subscribe) correlation -----------------------------------
  void add_stream(std::uint64_t id, const std::string &target,
                  grpc_session *owner, msg_fn on_msg, end_fn on_end) {
    m_streams[id] =
        stream_req{target, owner, std::move(on_msg), std::move(on_end)};
  }
  void cancel_stream(std::uint64_t id) { m_streams.erase(id); }
  // Drop streams owned by an operator connection that ended.
  void drop_streams_owned_by(grpc_session *owner) {
    for (auto it = m_streams.begin(); it != m_streams.end();) {
      if (it->second.owner == owner)
        it = m_streams.erase(it);
      else
        ++it;
    }
  }

  // ---- target reply routing (from the Session message handler) --------------
  void on_target_payload(std::uint64_t id, const std::string &payload) {
    if (auto p = m_pending.find(id); p != m_pending.end()) {
      reply_fn cb = std::move(p->second.cb);
      m_pending.erase(p);
      cb(0, payload); // OK
      return;
    }
    if (auto s = m_streams.find(id); s != m_streams.end())
      s->second.on_msg(payload); // relay one Subscribe response, keep the stream
  }
  void on_target_error(std::uint64_t id, const std::string & /*msg*/) {
    if (auto p = m_pending.find(id); p != m_pending.end()) {
      reply_fn cb = std::move(p->second.cb);
      m_pending.erase(p);
      cb(2, ""); // UNKNOWN
      return;
    }
    if (auto s = m_streams.find(id); s != m_streams.end()) {
      end_fn cb = std::move(s->second.on_end);
      m_streams.erase(s);
      cb(13); // INTERNAL
    }
  }

private:
  tunnel_hub() = default;

  // A target vanished: fail every pending/stream routed to it (erase first so
  // the callbacks can't re-enter a map being iterated).
  void fail_target(const std::string &target) {
    for (auto it = m_pending.begin(); it != m_pending.end();) {
      if (it->second.target == target) {
        reply_fn cb = std::move(it->second.cb);
        it = m_pending.erase(it);
        cb(14, ""); // UNAVAILABLE
      } else {
        ++it;
      }
    }
    for (auto it = m_streams.begin(); it != m_streams.end();) {
      if (it->second.target == target) {
        end_fn cb = std::move(it->second.on_end);
        it = m_streams.erase(it);
        cb(14); // UNAVAILABLE
      } else {
        ++it;
      }
    }
  }

  struct pending_req {
    std::string target;
    reply_fn cb;
  };
  struct stream_req {
    std::string target;
    grpc_session *owner{nullptr};
    msg_fn on_msg;
    end_fn on_end;
  };

  std::unordered_map<std::string, session> m_sessions;
  std::unordered_map<std::uint64_t, pending_req> m_pending;
  std::unordered_map<std::uint64_t, stream_req> m_streams;
  std::uint64_t m_next_id{0};
};

#endif // __tunnel_hub_hpp__
