#ifndef __openvpn_server_cpp__
#define __openvpn_server_cpp__

#include "openvpn_server.hpp"
#include "openvpn_peer.hpp"

#include <arpa/inet.h>
#include <iostream>

// ---------------------------------------------------------------------------
// ip_pool
// ---------------------------------------------------------------------------

uint32_t ip_pool::to_u32(const std::string &ip) {
  struct in_addr a{};
  inet_pton(AF_INET, ip.c_str(), &a);
  return ntohl(a.s_addr); // host byte order for arithmetic comparison
}

std::string ip_pool::to_str(uint32_t addr) {
  struct in_addr a{};
  a.s_addr = htonl(addr);
  char buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}

ip_pool::ip_pool(const std::string &start_ip, const std::string &end_ip) {
  const uint32_t lo = to_u32(start_ip);
  const uint32_t hi = to_u32(end_ip);
  for (uint32_t a = lo; a <= hi; ++a)
    m_free.insert(a);
}

std::string ip_pool::assign(int32_t channel) {
  if (m_free.empty()) return {};
  const uint32_t addr = *m_free.begin();
  m_free.erase(m_free.begin());
  m_assigned[channel] = addr;
  return to_str(addr);
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
  return it != m_assigned.end() ? to_str(it->second) : std::string{};
}

// ---------------------------------------------------------------------------
// openvpn_server
// ---------------------------------------------------------------------------

openvpn_server::openvpn_server(const std::string &host, uint16_t port,
                                 const std::string &pool_start,
                                 const std::string &pool_end,
                                 const tls_config  &tls)
    : evt_io(host, port, tls.build_server_ctx(), listener_tag{}),
      m_pool(pool_start, pool_end) {
  std::cout << "[openvpn_server] " << host << ":" << port
            << " pool=" << pool_start << "–" << pool_end
            << " tls=" << (tls.enabled ? "ON" : "OFF") << '\n';
}

openvpn_server::~openvpn_server() {
  m_peers.clear();
}

std::int32_t openvpn_server::handle_connect(const handle_t &channel,
                                              const std::string &peer_host) {
  const std::string ip = m_pool.assign(channel);
  if (ip.empty()) {
    std::cerr << "[openvpn_server] pool exhausted, rejecting " << peer_host << '\n';
    return -1;
  }

  // wrap_accepted() is in evt_io: returns a TLS bev when the server was
  // constructed with a TLS ctx, plain socket bev otherwise.
  auto *bev = wrap_accepted(channel);
  auto peer = std::make_unique<openvpn_peer>(bev, peer_host, this, ip);
  m_peers.emplace(channel, std::move(peer));
  std::cout << "[openvpn_server] accepted " << peer_host
            << " \xe2\x86\x92 " << ip << (has_tls() ? " (TLS)" : "") << '\n';
  return 0;
}

std::int32_t openvpn_server::handle_close(const handle_t &channel) {
  m_peers.erase(channel);
  return 0;
}

#endif // __openvpn_server_cpp__
