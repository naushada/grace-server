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
#include <net/route.h>
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
  return ntohl(a.s_addr);
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

int32_t ip_pool::find_channel(const std::string &ip) const {
  const uint32_t addr = to_u32(ip);
  for (const auto &[ch, a] : m_assigned)
    if (a == addr) return ch;
  return -1;
}

// ---------------------------------------------------------------------------
// openvpn_server
// ---------------------------------------------------------------------------

openvpn_server::openvpn_server(const std::string  &host,
                                 uint16_t            port,
                                 const std::string  &pool_start,
                                 const std::string  &pool_end,
                                 const tls_config   &tls,
                                 const std::string  &server_ip,
                                 const std::string  &netmask,
                                 const mqtt_sub_cfg &mqtt_sub)
    : evt_io(host, port, tls.build_server_ctx(), listener_tag{}),
      m_pool(pool_start, pool_end), m_netmask(netmask),
      m_mqtt_cfg(mqtt_sub) {
  std::cout << "[openvpn_server] " << host << ":" << port
            << " pool=" << pool_start << "\xe2\x80\x93" << pool_end
            << " tls=" << (tls.enabled ? "ON" : "OFF")
            << " mqtt=" << (mqtt_sub.enabled ? "ON" : "OFF") << '\n';
  open_server_tun(server_ip);
}

openvpn_server::~openvpn_server() {
  m_server_tun_io.reset();
  if (m_server_tun_fd >= 0) ::close(m_server_tun_fd);
  m_peers.clear();
}

// ---------------------------------------------------------------------------
// Connection handling
// ---------------------------------------------------------------------------

std::int32_t openvpn_server::handle_connect(const handle_t &channel,
                                              const std::string &peer_host) {
  const std::string ip = m_pool.assign(channel);
  if (ip.empty()) {
    std::cerr << "[openvpn_server] pool exhausted, rejecting " << peer_host << '\n';
    return -1;
  }

  auto *bev = wrap_accepted(channel);
  // Pass the MQTT broker config so the peer subscribes to fwd/<vip> itself.
  auto peer = std::make_unique<openvpn_peer>(bev, peer_host, this, ip,
                                              m_netmask, m_mqtt_cfg);
  m_peers.emplace(channel, std::move(peer));
  manage_client_route(ip, true);
  std::cout << "[openvpn_server] accepted " << peer_host
            << " \xe2\x86\x92 " << ip << (has_tls() ? " (TLS)" : "") << '\n';
  return 0;
}

std::int32_t openvpn_server::handle_close(const handle_t &channel) {
  const std::string ip = m_pool.get(channel);
  if (!ip.empty()) manage_client_route(ip, false);
  m_pool.release(channel);
  m_peers.erase(channel);
  return 0;
}

// ---------------------------------------------------------------------------
// Server TUN — routes inbound IP packets to the right peer
// ---------------------------------------------------------------------------

class openvpn_server::server_tun_io : public evt_io {
public:
  server_tun_io(evutil_socket_t fd, openvpn_server &owner)
      : evt_io(fd, "tun"), m_owner(owner) {}

  std::int32_t handle_read(const std::int32_t &, const std::string &data,
                            const bool &dry_run) override {
    if (dry_run || data.size() < 20) return 0;
    char dst[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, data.data() + 16, dst, sizeof(dst));
    const int32_t ch = m_owner.m_pool.find_channel(dst);
    if (ch >= 0) {
      auto it = m_owner.m_peers.find(ch);
      if (it != m_owner.m_peers.end())
        it->second->forward_data(data);
    }
    return 0;
  }
private:
  openvpn_server &m_owner;
};

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
  m_server_tun_name = ifr.ifr_name;
  int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  auto *sin = reinterpret_cast<struct sockaddr_in *>(&ifr.ifr_addr);
  sin->sin_family = AF_INET;
  inet_pton(AF_INET, server_ip.c_str(), &sin->sin_addr);
  ::ioctl(sock, SIOCSIFADDR, &ifr);
  inet_pton(AF_INET, m_netmask.c_str(), &sin->sin_addr);
  ::ioctl(sock, SIOCSIFNETMASK, &ifr);
  ::ioctl(sock, SIOCGIFFLAGS, &ifr);
  ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
  ::ioctl(sock, SIOCSIFFLAGS, &ifr);
  ::close(sock);
  std::cout << "[openvpn_server] tun " << m_server_tun_name
            << " configured: " << server_ip << "/24 UP\n";
  m_server_tun_io = std::make_unique<server_tun_io>(m_server_tun_fd, *this);
#endif
  return 0;
}

void openvpn_server::manage_client_route(const std::string &client_ip, bool add) {
#ifdef __linux__
  if (m_server_tun_name.empty()) return;
  struct rtentry rt{};
  auto set_addr = [](struct sockaddr &sa, const std::string &ip) {
    auto *s = reinterpret_cast<struct sockaddr_in *>(&sa);
    s->sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &s->sin_addr);
  };
  set_addr(rt.rt_dst,     client_ip);
  set_addr(rt.rt_genmask, "255.255.255.255");
  rt.rt_flags = RTF_UP | RTF_HOST;
  rt.rt_dev   = const_cast<char *>(m_server_tun_name.c_str());
  int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (::ioctl(sock, add ? SIOCADDRT : SIOCDELRT, &rt) < 0)
    std::cerr << "[openvpn_server] route " << (add ? "add" : "del")
              << " " << client_ip << ": " << strerror(errno) << '\n';
  else
    std::cout << "[openvpn_server] route " << (add ? "added" : "removed")
              << " host " << client_ip << " dev " << m_server_tun_name << '\n';
  ::close(sock);
#endif
}

#endif // __openvpn_server_cpp__
