// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#ifndef __tunnel_hub_hpp__
#define __tunnel_hub_hpp__

// grpctunnel state: registered targets + active data bridges.
//
// Register (control): a tunnel client (device) sends Target{ADD,target,type};
// recorded here with its Register stream so the server can send Session{tag}.
//
// Data plane (Increment B): an operator connects to the server's local gNMI
// listener → open_bridge() allocates a tag and sends Session{tag,target} on the
// target's Register stream. The device acks and opens a Tunnel(stream Data) RPC;
// its first Data{tag} pairs the Tunnel stream to the bridge (pair_tunnel). Bytes
// then relay: operator socket ⇄ Data{tag} ⇄ device. Single-threaded — no lock.

#include "grpc_session.hpp"
#include "tunnel/tunnel.pb.h"

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class tunnel_hub {
public:
  using bytes_fn = std::function<void(const std::string &)>;
  using close_fn = std::function<void()>;

  struct target_info {
    std::string target;
    std::string type;
    std::time_t since{0};
    std::uint16_t local_port{0}; // configured local listener port (0 = none)
  };

  static tunnel_hub &instance() {
    static tunnel_hub hub;
    return hub;
  }

  // ---- Register: target registry -------------------------------------------
  void add_target(const std::string &target, const std::string &type,
                  grpc_session *g, std::int32_t sid, std::time_t now) {
    m_targets[target] = reg{g, sid, type, now};
  }
  void remove_target(const std::string &target) { m_targets.erase(target); }

  // A Register connection closed: drop its targets AND tear down any bridges to
  // them (close the operator side). Returns removed target ids.
  std::vector<std::string> remove_by_stream(grpc_session *g) {
    std::vector<std::string> gone;
    for (auto it = m_targets.begin(); it != m_targets.end();) {
      if (it->second.grpc == g) {
        gone.push_back(it->first);
        it = m_targets.erase(it);
      } else {
        ++it;
      }
    }
    for (const auto &t : gone) {
      for (auto it = m_bridges.begin(); it != m_bridges.end();) {
        if (it->second.target == t) {
          close_fn co = it->second.close_operator;
          it = m_bridges.erase(it);
          if (co) co();
        } else {
          ++it;
        }
      }
    }
    return gone;
  }

  bool has(const std::string &target) const {
    return m_targets.find(target) != m_targets.end();
  }
  std::size_t size() const { return m_targets.size(); }

  // Record the local listener port configured for a target (for the monitor).
  void set_listener_port(const std::string &target, std::uint16_t port) {
    m_ports[target] = port;
  }

  std::vector<target_info> snapshot() const {
    std::vector<target_info> out;
    out.reserve(m_targets.size());
    for (const auto &kv : m_targets) {
      std::uint16_t port = 0;
      if (auto pit = m_ports.find(kv.first); pit != m_ports.end())
        port = pit->second;
      out.push_back({kv.first, kv.second.type, kv.second.since, port});
    }
    return out;
  }

  // ---- Data plane: bridges --------------------------------------------------
  // Operator connected. Allocate a tag, remember how to write/close the operator
  // socket, and ask the target for a data session. Returns the tag, or 0 if the
  // target isn't connected.
  std::int32_t open_bridge(const std::string &target, bytes_fn to_operator,
                           close_fn close_operator) {
    auto tit = m_targets.find(target);
    if (tit == m_targets.end())
      return 0;
    const std::int32_t tag = ++m_next_tag;
    m_bridges[tag] =
        bridge{tag, target, std::move(to_operator), std::move(close_operator),
               nullptr, 0, false, ""};
    grpctunnel::RegisterOp op;
    grpctunnel::Session *s = op.mutable_session();
    s->set_tag(tag);
    s->set_target(target);
    s->set_target_type(tit->second.type);
    std::string pb;
    op.SerializeToString(&pb);
    tit->second.grpc->stream_send(tit->second.stream_id, pb);
    return tag;
  }

  // Device opened Tunnel(tag) — pair its stream to the bridge and flush any
  // operator bytes buffered before pairing.
  void pair_tunnel(std::int32_t tag, grpc_session *g, std::int32_t sid) {
    auto it = m_bridges.find(tag);
    if (it == m_bridges.end())
      return;
    it->second.tunnel_grpc = g;
    it->second.tunnel_sid = sid;
    it->second.paired = true;
    if (!it->second.buffered.empty()) {
      send_data(it->second, it->second.buffered);
      it->second.buffered.clear();
    }
  }

  // Operator → device bytes (buffer until the Tunnel stream is paired).
  void from_operator(std::int32_t tag, const std::string &bytes) {
    auto it = m_bridges.find(tag);
    if (it == m_bridges.end())
      return;
    if (!it->second.paired)
      it->second.buffered += bytes;
    else
      send_data(it->second, bytes);
  }

  // Device Data{tag,data} → operator socket.
  void from_device(std::int32_t tag, const std::string &bytes) {
    auto it = m_bridges.find(tag);
    if (it != m_bridges.end() && it->second.to_operator)
      it->second.to_operator(bytes);
  }

  // Operator socket closed: tell the device (Data{close}) and drop the bridge.
  void close_from_operator(std::int32_t tag) {
    auto it = m_bridges.find(tag);
    if (it == m_bridges.end())
      return;
    if (it->second.paired)
      send_close(it->second);
    m_bridges.erase(it);
  }

  // Device closed the tunnel (Data{close} or stream ended): close the operator
  // socket and drop the bridge.
  void close_from_device(std::int32_t tag) {
    auto it = m_bridges.find(tag);
    if (it == m_bridges.end())
      return;
    close_fn co = it->second.close_operator;
    m_bridges.erase(it);
    if (co)
      co();
  }

private:
  tunnel_hub() = default;

  struct reg {
    grpc_session *grpc{nullptr};
    std::int32_t stream_id{0};
    std::string type;
    std::time_t since{0};
  };
  struct bridge {
    std::int32_t tag{0};
    std::string target;
    bytes_fn to_operator;
    close_fn close_operator;
    grpc_session *tunnel_grpc{nullptr};
    std::int32_t tunnel_sid{0};
    bool paired{false};
    std::string buffered; // operator bytes before the Tunnel stream is paired
  };

  void send_data(bridge &b, const std::string &bytes) {
    if (!b.paired || !b.tunnel_grpc)
      return;
    grpctunnel::Data d;
    d.set_tag(b.tag);
    d.set_data(bytes);
    std::string pb;
    d.SerializeToString(&pb);
    b.tunnel_grpc->stream_send(b.tunnel_sid, pb);
  }
  void send_close(bridge &b) {
    if (!b.paired || !b.tunnel_grpc)
      return;
    grpctunnel::Data d;
    d.set_tag(b.tag);
    d.set_close(true);
    std::string pb;
    d.SerializeToString(&pb);
    b.tunnel_grpc->stream_send(b.tunnel_sid, pb);
  }

  std::unordered_map<std::string, reg> m_targets;
  std::unordered_map<std::string, std::uint16_t> m_ports; // target -> local port
  std::unordered_map<std::int32_t, bridge> m_bridges;     // tag -> bridge
  std::int32_t m_next_tag{0};
};

#endif // __tunnel_hub_hpp__
