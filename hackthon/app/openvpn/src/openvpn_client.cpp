#ifndef __openvpn_client_cpp__
#define __openvpn_client_cpp__

#include "openvpn_client.hpp"
#include "lua_engine.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>

#ifdef __linux__
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// make_bev — builds outbound bufferevent before base-ctor runs
// ---------------------------------------------------------------------------

struct bufferevent *openvpn_client::make_bev(const std::string &host,
                                               uint16_t port,
                                               SSL_CTX *ssl_ctx) {
  struct bufferevent *bev = nullptr;
  const struct timeval tv{5, 0};

  if (ssl_ctx) {
    SSL *ssl = SSL_new(ssl_ctx);
    bev = bufferevent_openssl_socket_new(evt_base::instance().get(), -1, ssl,
                                          BUFFEREVENT_SSL_CONNECTING,
                                          BEV_OPT_CLOSE_ON_FREE);
  } else {
    bev = bufferevent_socket_new(evt_base::instance().get(), -1,
                                  BEV_OPT_CLOSE_ON_FREE);
  }

  bufferevent_set_timeouts(bev, &tv, &tv);
  bufferevent_socket_connect_hostname(bev, nullptr, AF_UNSPEC,
                                       host.c_str(), port);
  return bev;
}

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

openvpn_client::openvpn_client(const std::string &host, uint16_t port,
                                 std::string status_file,
                                 const tls_config &tls)
    : evt_io(make_bev(host, port,
                       tls.enabled ? tls.build_client_ctx() : nullptr),
              host),
      m_status_file(std::move(status_file)),
      m_ssl_ctx(tls.enabled ? tls.build_client_ctx() : nullptr) {
  std::cout << "[openvpn_client] connecting to " << host << ":" << port
            << " tls=" << (tls.enabled ? "ON" : "OFF") << "\n";
}

openvpn_client::~openvpn_client() {
  close_tun();
  if (m_ssl_ctx) SSL_CTX_free(m_ssl_ctx);
}

// ---------------------------------------------------------------------------
// evt_io hooks
// ---------------------------------------------------------------------------

std::int32_t openvpn_client::handle_connect(const std::int32_t & /*ch*/,
                                              const std::string &peer) {
  std::cout << "[openvpn_client] connected to " << peer
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
  return 0;
}

std::int32_t openvpn_client::handle_event(const std::int32_t & /*ch*/,
                                            const std::uint16_t & /*ev*/) {
  std::cerr << "[openvpn_client] timeout\n";
  if (m_ip_assigned)
    write_status_lua(m_status_file, m_assigned_ip, m_tun_name, "Down",
                     std::time(nullptr));
  close_tun();
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
    case TYPE_IP_ASSIGN:
      m_assigned_ip = p;
      m_ip_assigned = true;
      std::cout << "[openvpn_client] IP_ASSIGN: " << m_assigned_ip << "\n";
      m_tun_fd = open_tun();
      if (m_tun_fd < 0)
        std::cerr << "[openvpn_client] TUN unavailable (need CAP_NET_ADMIN)\n";
      else
        std::cout << "[openvpn_client] kernel assigned " << m_tun_name << "\n";
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

void openvpn_client::close_tun() {
#ifdef __linux__
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
