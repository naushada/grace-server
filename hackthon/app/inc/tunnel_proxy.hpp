// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#ifndef __tunnel_proxy_hpp__
#define __tunnel_proxy_hpp__

// Local gNMI listener for the grpctunnel data plane (server-listens model).
//
// An operator/app connects to this listener; each connection is byte-proxied to
// a registered target over a Tunnel(stream Data) RPC. One listener fronts one
// target (the device that Registered it). On accept a tunnel_bridge is created,
// which asks tunnel_hub to open a session (Session{tag} on the target's Register
// stream) and then relays bytes both ways.

#include "framework.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>

class tunnel_bridge;

class tunnel_gnmi_listener : public evt_io {
public:
  tunnel_gnmi_listener(const std::string &host, std::uint16_t port,
                       std::string target)
      : evt_io(host, port), m_target(std::move(target)) {}
  ~tunnel_gnmi_listener() override { m_conns.clear(); }

  std::int32_t handle_connect(const std::int32_t &channel,
                              const std::string &peer_host) override;
  std::int32_t handle_close(const std::int32_t &channel) override;

private:
  std::string m_target;
  std::unordered_map<std::int32_t, std::unique_ptr<tunnel_bridge>> m_conns;
};

// One operator connection, bridged to the target over the tunnel by tag.
class tunnel_bridge : public evt_io {
public:
  tunnel_bridge(struct bufferevent *bev, const std::string &peer,
                tunnel_gnmi_listener *parent, const std::string &target,
                std::int32_t fd);

  std::int32_t handle_read(const std::int32_t &channel, const std::string &data,
                           const bool &dry_run) override;
  std::int32_t handle_close(const std::int32_t &channel) override;

  std::int32_t tag() const { return m_tag; }

private:
  tunnel_gnmi_listener *m_parent;
  std::int32_t m_fd;
  std::int32_t m_tag{0};
};

#endif // __tunnel_proxy_hpp__
