#ifndef __openvpn_server_cpp__
#define __openvpn_server_cpp__

#include "openvpn_server.hpp"
#include "openvpn_peer.hpp"

#include <iostream>

// ---------------------------------------------------------------------------
// ip_pool
// ---------------------------------------------------------------------------

ip_pool::ip_pool(const std::string &network, uint8_t start, uint8_t end)
    : m_network(network) {
  for (uint8_t h = start; h <= end; ++h)
    m_free.insert(h);
}

std::string ip_pool::assign(int32_t channel) {
  if (m_free.empty())
    return {};
  const uint8_t octet = *m_free.begin();
  m_free.erase(m_free.begin());
  m_assigned[channel] = octet;
  return m_network + "." + std::to_string(octet);
}

void ip_pool::release(int32_t channel) {
  auto it = m_assigned.find(channel);
  if (it != m_assigned.end()) {
    m_free.insert(it->second);
    m_assigned.erase(it);
  }
}

std::string ip_pool::get(int32_t channel) const {
  auto it = m_assigned.find(channel);
  return (it != m_assigned.end())
             ? m_network + "." + std::to_string(it->second)
             : std::string{};
}

// ---------------------------------------------------------------------------
// openvpn_server
// ---------------------------------------------------------------------------

openvpn_server::openvpn_server(const std::string &host, uint16_t port,
                                 const std::string &pool_network,
                                 uint8_t pool_start, uint8_t pool_end)
    : evt_io(host, port),
      m_pool(pool_network, pool_start, pool_end) {
  std::cout << "[openvpn_server] listening " << host << ":" << port
            << "  pool=" << pool_network << "." << int(pool_start)
            << "-" << pool_network << "." << int(pool_end) << "\n";
}

std::int32_t openvpn_server::handle_connect(const handle_t &channel,
                                              const std::string &peer_host) {
  const std::string ip = m_pool.assign(channel);
  if (ip.empty()) {
    std::cerr << "[openvpn_server] pool exhausted, rejecting " << peer_host << "\n";
    return -1;
  }
  auto peer = std::make_unique<openvpn_peer>(channel, peer_host, this, ip);
  m_peers.emplace(channel, std::move(peer));
  std::cout << "[openvpn_server] accepted " << peer_host << " → " << ip << "\n";
  return 0;
}

std::int32_t openvpn_server::handle_close(const handle_t &channel) {
  m_peers.erase(channel);
  return 0;
}

#endif // __openvpn_server_cpp__
