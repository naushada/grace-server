#ifndef __openvpn_client_cpp__
#define __openvpn_client_cpp__

#include "openvpn_client.hpp"
#include "lua_engine.hpp"

#include <cstring>
#include <iostream>

#ifdef __linux__
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/sockios.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <event2/bufferevent.h>

// tun_io — wraps the TUN fd in evt_io so libevent delivers reads via handle_read.
class openvpn_client::tun_io : public evt_io {
public:
  tun_io(evutil_socket_t fd, openvpn_client &owner)
      : evt_io(fd, "tun"), m_owner(owner) {}

  std::int32_t handle_read(const std::int32_t &, const std::string &data,
                            const bool &dry_run) override {
    if (!dry_run && !data.empty())
      m_owner.send_frame(openvpn_client::TYPE_DATA, data);
    return 0;
  }
private:
  openvpn_client &m_owner;
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

// evt_io(host, port, ssl_ctx_ptr) handles both plain TCP (null ctx) and TLS
// (non-null ctx) outbound connections — no TLS wiring needed here.
openvpn_client::openvpn_client(const std::string &host, uint16_t port,
                                 std::string status_file,
                                 const tls_config &tls)
    : evt_io(host, port, tls.build_client_ctx()),
      m_server_host(host), m_server_port(port), m_tls(tls),
      m_status_file(std::move(status_file)) {
  std::cout << "[openvpn_client] connecting to " << host << ":" << port
            << " tls=" << (tls.enabled ? "ON" : "OFF") << '\n';
}

openvpn_client::~openvpn_client() {
  close_tun();
}

// ---------------------------------------------------------------------------
// evt_io hooks
// ---------------------------------------------------------------------------

std::int32_t openvpn_client::handle_connect(const std::int32_t &ch,
                                              const std::string & /*peer*/) {
  // client_event_cb passes an empty peer string — resolve actual IP from fd.
  char peer_ip[INET6_ADDRSTRLEN] = "<unknown>";
  struct sockaddr_storage ss{};
  socklen_t len = sizeof(ss);
  if (::getpeername(ch, reinterpret_cast<struct sockaddr *>(&ss), &len) == 0) {
    if (ss.ss_family == AF_INET)
      ::inet_ntop(AF_INET,
                  &reinterpret_cast<struct sockaddr_in *>(&ss)->sin_addr,
                  peer_ip, sizeof(peer_ip));
    else if (ss.ss_family == AF_INET6)
      ::inet_ntop(AF_INET6,
                  &reinterpret_cast<struct sockaddr_in6 *>(&ss)->sin6_addr,
                  peer_ip, sizeof(peer_ip));
  }
  // The 5s timeout was only needed for the initial connect attempt.
  // Clear it now so an idle established tunnel is not torn down.
  bufferevent_set_timeouts(get_bufferevt(), nullptr, nullptr);

  std::cout << "[openvpn_client] connected to " << m_server_host
            << " (" << peer_ip << "):" << m_server_port
            << ", waiting for IP_ASSIGN\n";
  return 0;
}

std::int32_t openvpn_client::handle_read(const std::int32_t & /*ch*/,
                                           const std::string &data,
                                           const bool &dry_run) {
  if (dry_run) return 0;
  m_recv_buf.append(data);
  const size_t consumed = process_frames();
  m_recv_buf.erase(0, consumed);
  return static_cast<int32_t>(consumed);
}

std::int32_t openvpn_client::handle_close(const std::int32_t & /*ch*/) {
  std::cerr << "[openvpn_client] connection closed\n";
  if (m_ip_assigned)
    write_status_lua(m_status_file, m_assigned_ip, m_tun_name, "Down",
                     std::time(nullptr));
  close_tun();
  m_ip_assigned = false;
  m_assigned_ip.clear();
  m_recv_buf.clear();
  schedule_reconnect();
  return 0;
}

std::int32_t openvpn_client::handle_event(const std::int32_t & /*ch*/,
                                            const std::uint16_t & /*ev*/) {
  std::cerr << "[openvpn_client] timeout/error\n";
  if (m_ip_assigned)
    write_status_lua(m_status_file, m_assigned_ip, m_tun_name, "Down",
                     std::time(nullptr));
  close_tun();
  m_ip_assigned = false;
  m_assigned_ip.clear();
  m_recv_buf.clear();
  schedule_reconnect();
  return 0;
}

void openvpn_client::schedule_reconnect() {
  std::cout << "[openvpn_client] reconnecting in " << RECONNECT_DELAY_S
            << "s...\n";
  arm_timer(TIMER_RECONNECT, {RECONNECT_DELAY_S, 0});
}

std::int32_t openvpn_client::handle_timeout(int /*timer_id*/) {
  std::cout << "[openvpn_client] retrying " << m_server_host
            << ":" << m_server_port << "\n";
  bufferevent_socket_connect_hostname(get_bufferevt(), nullptr,
                                      AF_UNSPEC,
                                      m_server_host.c_str(),
                                      m_server_port);
  return 0;
}

std::int32_t openvpn_client::handle_write(const std::int32_t & /*ch*/) {
  return 0;
}

// ---------------------------------------------------------------------------
// Frame helpers
// ---------------------------------------------------------------------------

std::string openvpn_client::encode_frame(uint8_t type,
                                           const std::string &payload) {
  const uint32_t len_be = htonl(static_cast<uint32_t>(payload.size()));
  std::string frame;
  frame.reserve(HEADER_LEN + payload.size());
  frame.push_back(static_cast<char>(type));
  frame.append(reinterpret_cast<const char *>(&len_be), 4);
  frame.append(payload);
  return frame;
}

bool openvpn_client::try_decode_frame(const std::string &buf, size_t offset,
                                       uint8_t &out_type,
                                       std::string &out_payload,
                                       size_t &out_consumed) {
  if (buf.size() - offset < HEADER_LEN) return false;
  uint32_t len_be = 0;
  std::memcpy(&len_be, buf.data() + offset + 1, 4);
  const uint32_t len = ntohl(len_be);
  if (buf.size() - offset < HEADER_LEN + len) return false;
  out_type     = static_cast<uint8_t>(buf[offset]);
  out_payload  = buf.substr(offset + HEADER_LEN, len);
  out_consumed = HEADER_LEN + len;
  return true;
}

void openvpn_client::send_frame(uint8_t type, const std::string &payload) {
  const auto f = encode_frame(type, payload);
  tx(f.data(), f.size());
}

size_t openvpn_client::process_frames() {
  size_t consumed = 0;
  while (true) {
    uint8_t t{}; std::string p{}; size_t used{};
    if (!try_decode_frame(m_recv_buf, consumed, t, p, used)) break;
    consumed += used;
    switch (t) {
    case TYPE_IP_ASSIGN: {
      const auto sp = p.find(' ');
      m_assigned_ip = p.substr(0, sp);
      const std::string netmask = (sp != std::string::npos) ? p.substr(sp + 1) : "255.255.255.0";
      m_ip_assigned = true;
      std::cout << "[openvpn_client] IP_ASSIGN: " << m_assigned_ip << " " << netmask << "\n";
      m_tun_fd = open_tun();
      if (m_tun_fd < 0)
        std::cerr << "[openvpn_client] TUN unavailable (need CAP_NET_ADMIN)\n";
      else {
        std::cout << "[openvpn_client] kernel assigned " << m_tun_name << "\n";
        assign_tun_ip(m_assigned_ip, netmask);
        m_tun_io = std::make_unique<tun_io>(m_tun_fd, *this);
      }
    }
      write_status_lua(m_status_file, m_assigned_ip, m_tun_name, "Connected",
                        std::time(nullptr));
      break;
    case TYPE_DATA:
#ifdef __linux__
      if (m_tun_fd >= 0) ::write(m_tun_fd, p.data(), p.size());
#endif
      break;
    case TYPE_DISCONNECT:
      std::cout << "[openvpn_client] DISCONNECT from server\n";
      write_status_lua(m_status_file, m_assigned_ip, m_tun_name, "Down",
                        std::time(nullptr));
      close_tun();
      break;
    default:
      std::cerr << "[openvpn_client] unknown frame 0x"
                << std::hex << int(t) << std::dec << "\n";
      break;
    }
  }
  return consumed;
}

// ---------------------------------------------------------------------------
// TUN interface — kernel chooses the name
// ---------------------------------------------------------------------------

int openvpn_client::open_tun() {
#ifdef __linux__
  int fd = ::open("/dev/net/tun", O_RDWR);
  if (fd < 0) { std::cerr << "[openvpn_client] open tun: " << strerror(errno) << "\n"; return -1; }
  struct ifreq ifr{};
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  // Empty name → kernel assigns next free tunX; reads actual name back.
  if (::ioctl(fd, TUNSETIFF, &ifr) < 0) {
    std::cerr << "[openvpn_client] TUNSETIFF: " << strerror(errno) << "\n";
    ::close(fd); return -1;
  }
  m_tun_name = ifr.ifr_name;
  return fd;
#else
  return -1;
#endif
}

void openvpn_client::assign_tun_ip(const std::string &ip, const std::string &netmask) {
#ifdef __linux__
  int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock < 0) {
    std::cerr << "[openvpn_client] assign_tun_ip: socket: " << strerror(errno) << "\n";
    return;
  }

  struct ifreq ifr{};
  strncpy(ifr.ifr_name, m_tun_name.c_str(), IFNAMSIZ - 1);
  auto *sin = reinterpret_cast<struct sockaddr_in *>(&ifr.ifr_addr);
  sin->sin_family = AF_INET;

  // IP address
  inet_pton(AF_INET, ip.c_str(), &sin->sin_addr);
  if (::ioctl(sock, SIOCSIFADDR, &ifr) < 0)
    std::cerr << "[openvpn_client] SIOCSIFADDR: " << strerror(errno) << "\n";

  // Netmask /24
  inet_pton(AF_INET, netmask.c_str(), &sin->sin_addr);
  if (::ioctl(sock, SIOCSIFNETMASK, &ifr) < 0)
    std::cerr << "[openvpn_client] SIOCSIFNETMASK: " << strerror(errno) << "\n";

  // Bring the interface up
  if (::ioctl(sock, SIOCGIFFLAGS, &ifr) == 0) {
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (::ioctl(sock, SIOCSIFFLAGS, &ifr) < 0)
      std::cerr << "[openvpn_client] SIOCSIFFLAGS: " << strerror(errno) << "\n";
  }

  ::close(sock);
  std::cout << "[openvpn_client] " << m_tun_name << " configured: " << ip << "/24 UP\n";
#endif
}

void openvpn_client::close_tun() {
#ifdef __linux__
  m_tun_io.reset();
  if (m_tun_fd >= 0) { ::close(m_tun_fd); m_tun_fd = -1; }
#endif
}

// ---------------------------------------------------------------------------
// Status Lua — all Lua file I/O via lua_file::write_table
// ---------------------------------------------------------------------------

void openvpn_client::write_status_lua(const std::string &path,
                                        const std::string &service_ip,
                                        const std::string &tun_if,
                                        const std::string &status,
                                        std::time_t        ts) {
  lua_file::write_table(path, "vpn_status", {
    {"service_ip", "\"" + service_ip + "\""},
    {"tun_if",     "\"" + tun_if     + "\""},
    {"status",     "\"" + status     + "\""},
    {"timestamp",  std::to_string(static_cast<long long>(ts))},
  });
}

#endif // __openvpn_client_cpp__
