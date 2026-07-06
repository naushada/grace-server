#ifndef __tunnel_hub_hpp__
#define __tunnel_hub_hpp__

// Registry of live gRPC-tunnel sessions (dial-out targets).
//
// A target dials into the tunnel server, opens the Session bidi stream, and
// sends a Register frame with its target_id. That id is recorded here alongside
// the owning grpc_session + stream so the server can later push TunnelRequest
// frames DOWN to a specific target. Single-threaded (libevent loop) — no lock.

#include "grpc_session.hpp"
#include "tunnel/tunnel.pb.h"

#include <cstdint>
#include <string>
#include <unordered_map>

class tunnel_hub {
public:
  struct session {
    grpc_session *grpc{nullptr};
    std::int32_t stream_id{0};
  };

  static tunnel_hub &instance() {
    static tunnel_hub hub;
    return hub;
  }

  // Register (or re-register) a target id against its open bidi stream.
  void add(const std::string &target_id, grpc_session *g, std::int32_t sid) {
    m_sessions[target_id] = session{g, sid};
  }

  // Drop every session owned by a grpc_session (called when a connection ends,
  // so we never hold a dangling pointer).
  void remove(grpc_session *g) {
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
      if (it->second.grpc == g)
        it = m_sessions.erase(it);
      else
        ++it;
    }
  }

  bool connected(const std::string &target_id) const {
    return m_sessions.find(target_id) != m_sessions.end();
  }
  std::size_t size() const { return m_sessions.size(); }

  // Push a TunnelRequest down the target's stream. Returns false if the target
  // is not currently connected.
  bool send(const std::string &target_id, const tunnel::TunnelRequest &req) {
    auto it = m_sessions.find(target_id);
    if (it == m_sessions.end() || it->second.grpc == nullptr)
      return false;
    std::string pb;
    req.SerializeToString(&pb);
    it->second.grpc->stream_send(it->second.stream_id, pb);
    return true;
  }

private:
  tunnel_hub() = default;
  std::unordered_map<std::string, session> m_sessions;
};

#endif // __tunnel_hub_hpp__
