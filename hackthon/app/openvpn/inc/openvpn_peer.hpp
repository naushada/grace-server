#ifndef __openvpn_peer_hpp__
#define __openvpn_peer_hpp__

#include "framework.hpp"

#include <cstdint>
#include <string>

class openvpn_server;

// Per-client tunnel connection handler.  Inherits evt_io (inbound / server-side
// constructor) so libevent delivers I/O events through the standard hooks.
//
// Frame format (shared with openvpn_tunnel_client):
//   [type : 1 byte][length : 4 bytes big-endian][payload : length bytes]
//
//   0x01  IP_ASSIGN   server → client   payload = assigned IP as ASCII string
//   0x02  DATA        bidirectional     payload = raw tunnel bytes
//   0x03  DISCONNECT  client → server   graceful shutdown notification
class openvpn_peer : public evt_io {
public:
  static constexpr uint8_t TYPE_IP_ASSIGN  = 0x01;
  static constexpr uint8_t TYPE_DATA       = 0x02;
  static constexpr uint8_t TYPE_DISCONNECT = 0x03;
  static constexpr size_t  HEADER_LEN      = 5; // 1 (type) + 4 (length)

  // Plain TCP: framework creates the bufferevent from the accepted fd.
  openvpn_peer(int32_t channel, const std::string &peer_host,
               openvpn_server *parent, const std::string &assigned_ip);

  // TLS: caller supplies the pre-built SSL bufferevent; uses the protected
  // evt_io(bufferevent*, peer_host) constructor.
  openvpn_peer(struct bufferevent *bev, const std::string &peer_host,
               openvpn_server *parent, const std::string &assigned_ip);

  virtual ~openvpn_peer() = default;

  const std::string &assigned_ip() const { return m_assigned_ip; }

  std::int32_t handle_read(const std::int32_t &channel,
                           const std::string &data,
                           const bool &dry_run) override;
  std::int32_t handle_close(const std::int32_t &channel) override;
  std::int32_t handle_event(const std::int32_t &channel,
                             const std::uint16_t &event) override;
  std::int32_t handle_write(const std::int32_t &channel) override;

private:
  void   send_ip_assign();
  void   send_frame(uint8_t type, const std::string &payload);
  size_t process_frames(const std::string &buf);

  openvpn_server *m_parent;
  std::string     m_assigned_ip;
  std::string     m_recv_buf; // reassembly buffer for partial frames
};

#endif // __openvpn_peer_hpp__
