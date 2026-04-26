#ifndef __openvpn_client_cpp__
#define __openvpn_client_cpp__

#include "openvpn_client.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <fstream>
#include <iostream>

#ifdef __linux__
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

openvpn_client::openvpn_client(const std::string &host, uint16_t port,
                                 std::string tun_name, std::string status_file)
    : evt_io(host, port, /*outbound=*/true),
      m_tun_name(std::move(tun_name)),
      m_status_file(std::move(status_file)) {
  std::cout << "[openvpn_client] connecting to " << host << ":" << port << "\n";
}

openvpn_client::~openvpn_client() {
  close_tun();
}

// ---------------------------------------------------------------------------
// evt_io hook overrides
// ---------------------------------------------------------------------------

std::int32_t openvpn_client::handle_connect(const std::int32_t & /*channel*/,
                                              const std::string &peer_host) {
  std::cout << "[openvpn_client] TCP connected to " << peer_host
            << ", waiting for IP_ASSIGN\n";
  return 0;
}

std::int32_t openvpn_client::handle_read(const std::int32_t & /*channel*/,
                                           const std::string &data,
                                           const bool &dry_run) {
  if (dry_run)
    return 0;

  m_recv_buf.append(data);
  const size_t consumed = process_frames();
  m_recv_buf.erase(0, consumed);
  return static_cast<int32_t>(consumed);
}

std::int32_t openvpn_client::handle_close(const std::int32_t & /*channel*/) {
  std::cerr << "[openvpn_client] connection closed\n";
  if (m_ip_assigned)
    write_status_lua(m_status_file, m_assigned_ip, "Down", std::time(nullptr));
  close_tun();
  return 0;
}

std::int32_t openvpn_client::handle_event(const std::int32_t & /*channel*/,
                                            const std::uint16_t & /*event*/) {
  std::cerr << "[openvpn_client] timeout\n";
  if (m_ip_assigned)
    write_status_lua(m_status_file, m_assigned_ip, "Down", std::time(nullptr));
  close_tun();
  return 0;
}

std::int32_t openvpn_client::handle_write(const std::int32_t & /*channel*/) {
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
  if (buf.size() - offset < HEADER_LEN)
    return false;
  uint32_t len_be = 0;
  std::memcpy(&len_be, buf.data() + offset + 1, 4);
  const uint32_t len = ntohl(len_be);
  if (buf.size() - offset < HEADER_LEN + len)
    return false;
  out_type     = static_cast<uint8_t>(buf[offset]);
  out_payload  = buf.substr(offset + HEADER_LEN, len);
  out_consumed = HEADER_LEN + len;
  return true;
}

void openvpn_client::send_frame(uint8_t type, const std::string &payload) {
  const auto frame = encode_frame(type, payload);
  tx(frame.data(), frame.size());
}

size_t openvpn_client::process_frames() {
  size_t consumed = 0;

  while (true) {
    uint8_t     type{};
    std::string payload;
    size_t      used{};

    if (!try_decode_frame(m_recv_buf, consumed, type, payload, used))
      break;
    consumed += used;

    switch (type) {
    case TYPE_IP_ASSIGN:
      m_assigned_ip  = payload;
      m_ip_assigned  = true;
      std::cout << "[openvpn_client] IP_ASSIGN received: " << m_assigned_ip << "\n";

      m_tun_fd = open_tun();
      if (m_tun_fd < 0)
        std::cerr << "[openvpn_client] WARNING: could not create "
                  << m_tun_name << " (need CAP_NET_ADMIN or Linux)\n";
      else
        std::cout << "[openvpn_client] created " << m_tun_name
                  << " with IP " << m_assigned_ip << "\n";

      write_status_lua(m_status_file, m_assigned_ip, "Connected",
                        std::time(nullptr));
      break;

    case TYPE_DATA:
      if (m_tun_fd >= 0) {
#ifdef __linux__
        ::write(m_tun_fd, payload.data(), payload.size());
#endif
      }
      break;

    case TYPE_DISCONNECT:
      std::cout << "[openvpn_client] server sent DISCONNECT\n";
      write_status_lua(m_status_file, m_assigned_ip, "Down",
                        std::time(nullptr));
      close_tun();
      break;

    default:
      std::cerr << "[openvpn_client] unknown frame type 0x"
                << std::hex << int(type) << std::dec << "\n";
      break;
    }
  }

  return consumed;
}

// ---------------------------------------------------------------------------
// TUN interface
// ---------------------------------------------------------------------------

int openvpn_client::open_tun() {
#ifdef __linux__
  int fd = ::open("/dev/net/tun", O_RDWR);
  if (fd < 0) {
    std::cerr << "[openvpn_client] open /dev/net/tun failed: "
              << std::strerror(errno) << "\n";
    return -1;
  }
  struct ifreq ifr{};
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  std::strncpy(ifr.ifr_name, m_tun_name.c_str(), IFNAMSIZ - 1);
  if (::ioctl(fd, TUNSETIFF, &ifr) < 0) {
    std::cerr << "[openvpn_client] TUNSETIFF failed: "
              << std::strerror(errno) << "\n";
    ::close(fd);
    return -1;
  }
  return fd;
#else
  return -1; // not supported on this platform
#endif
}

void openvpn_client::close_tun() {
  if (m_tun_fd >= 0) {
#ifdef __linux__
    ::close(m_tun_fd);
#endif
    m_tun_fd = -1;
  }
}

// ---------------------------------------------------------------------------
// Status Lua writer
// ---------------------------------------------------------------------------

void openvpn_client::write_status_lua(const std::string &path,
                                        const std::string &service_ip,
                                        const std::string &status,
                                        std::time_t        timestamp) {
  std::ofstream f(path, std::ios::trunc);
  if (!f) {
    std::cerr << "[openvpn_client] cannot write status file: " << path << "\n";
    return;
  }
  f << "-- VPN connection status — written by openvpn_client\n"
    << "return {\n"
    << "  vpn_status = {\n"
    << "    service_ip = \"" << service_ip << "\",\n"
    << "    status     = \"" << status << "\",\n"
    << "    timestamp  = "  << static_cast<long long>(timestamp) << ",\n"
    << "  },\n"
    << "}\n";
  std::cout << "[openvpn_client] status=" << status
            << " ip=" << service_ip
            << " written to " << path << "\n";
}

#endif // __openvpn_client_cpp__
