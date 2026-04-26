#ifndef __openvpn_peer_cpp__
#define __openvpn_peer_cpp__

#include "openvpn_peer.hpp"
#include "openvpn_server.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>

// ---------------------------------------------------------------------------
// Constructor — send IP_ASSIGN immediately after the connection is created.
// ---------------------------------------------------------------------------

openvpn_peer::openvpn_peer(int32_t channel, const std::string &peer_host,
                             openvpn_server *parent,
                             const std::string &assigned_ip)
    : evt_io(channel, peer_host),
      m_parent(parent),
      m_assigned_ip(assigned_ip) {
  send_ip_assign();
}

// ---------------------------------------------------------------------------
// Frame helpers
// ---------------------------------------------------------------------------

void openvpn_peer::send_frame(uint8_t type, const std::string &payload) {
  const uint32_t len    = static_cast<uint32_t>(payload.size());
  const uint32_t len_be = htonl(len);
  std::string    frame;
  frame.reserve(HEADER_LEN + len);
  frame.push_back(static_cast<char>(type));
  frame.append(reinterpret_cast<const char *>(&len_be), 4);
  frame.append(payload);
  tx(frame.data(), frame.size());
}

void openvpn_peer::send_ip_assign() {
  send_frame(TYPE_IP_ASSIGN, m_assigned_ip);
  std::cout << "[openvpn_peer] IP_ASSIGN " << m_assigned_ip << "\n";
}

// ---------------------------------------------------------------------------
// Frame reassembly — consume complete frames from buf, return bytes used.
// ---------------------------------------------------------------------------

size_t openvpn_peer::process_frames(const std::string &buf) {
  size_t consumed = 0;

  while (buf.size() - consumed >= HEADER_LEN) {
    const uint8_t type = static_cast<uint8_t>(buf[consumed]);
    uint32_t len_be = 0;
    std::memcpy(&len_be, buf.data() + consumed + 1, 4);
    const uint32_t len = ntohl(len_be);

    if (buf.size() - consumed < HEADER_LEN + len)
      break; // incomplete frame — hold in recv_buf

    const std::string payload = buf.substr(consumed + HEADER_LEN, len);
    consumed += HEADER_LEN + len;

    switch (type) {
    case TYPE_DATA:
      std::cout << "[openvpn_peer] DATA " << len
                << " bytes from " << m_assigned_ip << "\n";
      // Forward payload to TUN interface in a real implementation.
      break;

    case TYPE_DISCONNECT:
      std::cout << "[openvpn_peer] DISCONNECT from " << m_assigned_ip << "\n";
      break;

    default:
      std::cerr << "[openvpn_peer] unknown frame type 0x"
                << std::hex << int(type) << std::dec << "\n";
      break;
    }
  }

  return consumed;
}

// ---------------------------------------------------------------------------
// evt_io hook overrides
// ---------------------------------------------------------------------------

std::int32_t openvpn_peer::handle_read(const std::int32_t & /*channel*/,
                                        const std::string &data,
                                        const bool &dry_run) {
  if (dry_run)
    return 0;

  m_recv_buf.append(data);
  const size_t consumed = process_frames(m_recv_buf);
  m_recv_buf.erase(0, consumed);
  return static_cast<int32_t>(consumed);
}

std::int32_t openvpn_peer::handle_close(const std::int32_t &channel) {
  std::cout << "[openvpn_peer] closed, releasing " << m_assigned_ip << "\n";
  // Release IP before notifying parent — parent erases this object.
  m_parent->pool().release(channel);
  m_parent->handle_close(channel);
  return 0;
}

std::int32_t openvpn_peer::handle_event(const std::int32_t &channel,
                                         const std::uint16_t & /*event*/) {
  std::cerr << "[openvpn_peer] timeout channel=" << channel
            << " ip=" << m_assigned_ip << "\n";
  m_parent->pool().release(channel);
  m_parent->handle_close(channel);
  return 0;
}

std::int32_t openvpn_peer::handle_write(const std::int32_t & /*channel*/) {
  return 0;
}

#endif // __openvpn_peer_cpp__
