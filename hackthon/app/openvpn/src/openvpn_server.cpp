#ifndef __openvpn_server_cpp__
#define __openvpn_server_cpp__

#include "openvpn_server.hpp"
#include "openvpn_peer.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>

#ifdef __linux__
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

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
                                 const tls_config  &tls,
                                 const std::string &server_ip)
    : evt_io(host, port, tls.build_server_ctx(), listener_tag{}),
      m_pool(pool_start, pool_end) {
  std::cout << "[openvpn_server] " << host << ":" << port
            << " pool=" << pool_start << "–" << pool_end
            << " tls=" << (tls.enabled ? "ON" : "OFF") << '\n';
  open_server_tun(server_ip);
}

openvpn_server::~openvpn_server() {
  if (m_server_tun_event) {
    event_del(m_server_tun_event);
    event_free(m_server_tun_event);
  }
  if (m_server_tun_fd >= 0) ::close(m_server_tun_fd);
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
  m_pool.release(channel);
  m_peers.erase(channel);
  return 0;
}

// ---------------------------------------------------------------------------
// ip_pool — reverse lookup: IP string → channel
// ---------------------------------------------------------------------------

int32_t ip_pool::find_channel(const std::string &ip) const {
  const uint32_t addr = to_u32(ip);
  for (const auto &[ch, a] : m_assigned)
    if (a == addr) return ch;
  return -1;
}

// ---------------------------------------------------------------------------
// Server TUN — receives IP packets from local stack, routes to right peer
// ---------------------------------------------------------------------------

int openvpn_server::open_server_tun(const std::string &server_ip) {
#ifdef __linux__
  m_server_tun_fd = ::open("/dev/net/tun", O_RDWR);
  if (m_server_tun_fd < 0) {
    std::cerr << "[openvpn_server] open tun: " << strerror(errno) << '\n';
    return -1;
  }
  struct ifreq ifr{};
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  if (::ioctl(m_server_tun_fd, TUNSETIFF, &ifr) < 0) {
    std::cerr << "[openvpn_server] TUNSETIFF: " << strerror(errno) << '\n';
    ::close(m_server_tun_fd); m_server_tun_fd = -1; return -1;
  }
  // Assign server IP and bring up
  int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  auto *sin = reinterpret_cast<struct sockaddr_in *>(&ifr.ifr_addr);
  sin->sin_family = AF_INET;
  inet_pton(AF_INET, server_ip.c_str(), &sin->sin_addr);
  ::ioctl(sock, SIOCSIFADDR, &ifr);
  inet_pton(AF_INET, "255.255.255.0", &sin->sin_addr);
  ::ioctl(sock, SIOCSIFNETMASK, &ifr);
  ::ioctl(sock, SIOCGIFFLAGS, &ifr);
  ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
  ::ioctl(sock, SIOCSIFFLAGS, &ifr);
  ::close(sock);
  std::cout << "[openvpn_server] tun " << ifr.ifr_name
            << " configured: " << server_ip << "/24 UP\n";
  // Register libevent reader
  m_server_tun_event = event_new(evt_base::instance().get(), m_server_tun_fd,
                                  EV_READ | EV_PERSIST, server_tun_read_cb, this);
  event_add(m_server_tun_event, nullptr);
#endif
  return 0;
}

void openvpn_server::server_tun_read_cb(evutil_socket_t fd, short, void *ctx) {
  auto *self = static_cast<openvpn_server *>(ctx);
  char buf[65536];
  ssize_t n = ::read(fd, buf, sizeof(buf));
  if (n < 20) return; // too short to be a valid IP packet

  // Extract destination IP from IP header (bytes 16-19)
  char dst[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, buf + 16, dst, sizeof(dst));

  const int32_t ch = self->m_pool.find_channel(dst);
  if (ch < 0) return; // no client owns this IP

  auto it = self->m_peers.find(ch);
  if (it != self->m_peers.end())
    it->second->forward_data(std::string(buf, static_cast<size_t>(n)));
}

#endif // __openvpn_server_cpp__
