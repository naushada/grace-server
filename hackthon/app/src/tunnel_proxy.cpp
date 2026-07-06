#include "tunnel_proxy.hpp"
#include "tunnel_hub.hpp"
#include "update_sink.hpp"

#include <iostream>

static void proxy_log(const std::string &line) {
  std::cout << line << "\n";
  update_sink::instance().emit(line);
}

// ---------------------------------------------------------------------------
// Listener: accept an operator connection, wrap it, and start a bridge.
// ---------------------------------------------------------------------------

std::int32_t tunnel_gnmi_listener::handle_connect(const std::int32_t &channel,
                                                  const std::string &peer_host) {
  auto *bev = wrap_accepted(channel); // plain (no TLS ctx on this listener)
  auto b = std::make_unique<tunnel_bridge>(bev, peer_host, this, m_target,
                                           channel);
  if (b->tag() == 0) {
    // No such target connected — drop the connection (bridge closes its bev).
    proxy_log("[tun] refused: target '" + m_target + "' not connected");
    return -1;
  }
  m_conns[channel] = std::move(b);
  return 0;
}

std::int32_t tunnel_gnmi_listener::handle_close(const std::int32_t &channel) {
  return static_cast<std::int32_t>(m_conns.erase(channel));
}

// ---------------------------------------------------------------------------
// Bridge: operator socket ⇄ Tunnel(Data) stream, relayed by tag.
// ---------------------------------------------------------------------------

tunnel_bridge::tunnel_bridge(struct bufferevent *bev, const std::string &peer,
                             tunnel_gnmi_listener *parent,
                             const std::string &target, std::int32_t fd)
    : evt_io(bev, peer), m_parent(parent), m_fd(fd) {
  m_tag = tunnel_hub::instance().open_bridge(
      target,
      // device → operator: write bytes to the operator socket.
      [this](const std::string &b) { this->tx(b.data(), b.size()); },
      // device closed the tunnel: drop this operator connection.
      [this]() {
        if (m_parent)
          m_parent->handle_close(m_fd);
      });
  if (m_tag != 0)
    proxy_log("[tun] bridge tag=" + std::to_string(m_tag) + " → '" + target +
              "'");
}

std::int32_t tunnel_bridge::handle_read(const std::int32_t & /*channel*/,
                                        const std::string &data,
                                        const bool &dry_run) {
  if (dry_run)
    return 0;
  if (m_tag != 0 && !data.empty())
    tunnel_hub::instance().from_operator(m_tag, data);
  return static_cast<std::int32_t>(data.size());
}

std::int32_t tunnel_bridge::handle_close(const std::int32_t &channel) {
  // Operator socket closed: tell the device, then remove ourselves from the
  // listener (which destroys this object — do it last).
  if (m_tag != 0)
    tunnel_hub::instance().close_from_operator(m_tag);
  if (m_parent)
    m_parent->handle_close(channel);
  return 0;
}
