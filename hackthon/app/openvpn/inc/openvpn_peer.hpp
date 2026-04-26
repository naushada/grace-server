#ifndef __openvpn_peer_hpp__
#define __openvpn_peer_hpp__

#include "framework.hpp"

#include <cstdint>
#include <string>

class openvpn_server;

// Per-client tunnel connection handler.  Inherits evt_io (inbound / server-side
// constructor) so libevent delivers I/O events through the standard hooks.
//
// ═══════════════════════════════════════════════════════════════════════════
// Tunnel frame format  (TCP stream between openvpn_server ↔ openvpn_client)
// ═══════════════════════════════════════════════════════════════════════════
//
//   ┌────────┬────────────────────────┬──────────────────────────────────┐
//   │ type   │ length (big-endian)    │ payload                          │
//   │ 1 byte │ 4 bytes                │ <length> bytes                   │
//   └────────┴────────────────────────┴──────────────────────────────────┘
//
//   type 0x01  IP_ASSIGN   direction: server → client (once, on connect)
//   type 0x02  DATA        direction: bidirectional
//   type 0x03  DISCONNECT  direction: client → server (graceful teardown)
//
// ───────────────────────────────────────────────────────────────────────────
// IP_ASSIGN payload  (type 0x01)
// ───────────────────────────────────────────────────────────────────────────
//   ASCII string:  "<ip> <netmask>\0"
//   Example:       "10.8.0.3 255.255.255.0"
//
//   The client splits on the first space:
//     field 0 → virtual IP  assigned to tunX  (SIOCSIFADDR)
//     field 1 → subnet mask applied to tunX   (SIOCSIFNETMASK)
//   After applying both, the client brings tunX UP|RUNNING.
//
// ───────────────────────────────────────────────────────────────────────────
// DATA payload  (type 0x02)
// ───────────────────────────────────────────────────────────────────────────
//   A single raw IPv4 packet, exactly as read from the TUN interface
//   (IFF_NO_PI — no prepended packet-information header).
//
//   Packet structure inside the payload:
//     ┌─────────────────────────────────────────────┐
//     │  IPv4 header  (min 20 bytes)                │
//     │    src  = sender's virtual IP (e.g. 10.8.0.3)
//     │    dst  = destination virtual IP            │
//     ├─────────────────────────────────────────────┤
//     │  Transport header  (TCP / UDP / ICMP …)     │
//     ├─────────────────────────────────────────────┤
//     │  Application payload                        │
//     └─────────────────────────────────────────────┘
//
//   Server-side routing:  server_tun_io reads the raw IP packet from its
//   tunX fd, extracts dst (bytes 16-19 of the IP header), looks up the
//   owning peer via ip_pool::find_channel(), and calls forward_data() to
//   wrap it in a DATA frame and send it over the TCP tunnel.
//
//   Client-side injection: openvpn_client::handle_read unwraps the DATA
//   frame and writes the raw IP packet to its tunX fd, injecting it into
//   the local kernel IP stack so applications see it as normal traffic.
//
// ───────────────────────────────────────────────────────────────────────────
// End-to-end flow example  (gNMI Get from server to client)
// ───────────────────────────────────────────────────────────────────────────
//   server app                tunnel                   client (10.8.0.3)
//   ──────────                ──────                   ─────────────────
//   gNMI → kernel → tunX → [DATA frame] → TCP → client tunX → kernel
//                                                              → gNMI svc
class openvpn_peer : public evt_io {
public:
  static constexpr uint8_t TYPE_IP_ASSIGN  = 0x01;
  static constexpr uint8_t TYPE_DATA       = 0x02;
  static constexpr uint8_t TYPE_DISCONNECT = 0x03;
  static constexpr size_t  HEADER_LEN      = 5; // 1 (type) + 4 (length)

  // Plain TCP: framework creates the bufferevent from the accepted fd.
  openvpn_peer(int32_t channel, const std::string &peer_host,
               openvpn_server *parent, const std::string &assigned_ip,
               const std::string &netmask);

  openvpn_peer(struct bufferevent *bev, const std::string &peer_host,
               openvpn_server *parent, const std::string &assigned_ip,
               const std::string &netmask);

  virtual ~openvpn_peer() = default;

  const std::string &assigned_ip() const { return m_assigned_ip; }
  void forward_data(const std::string &pkt) { send_frame(TYPE_DATA, pkt); }

  // Called when the TLS handshake completes (BEV_EVENT_CONNECTED).
  // Extracts the client certificate CN and logs it; rejects the connection
  // if the CN is not in the server's allowed-CN list.
  std::int32_t handle_connect(const std::int32_t &channel,
                               const std::string &peer_host) override;
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

  static std::string extract_cn(struct bufferevent *bev);

  openvpn_server *m_parent;
  std::string     m_assigned_ip;
  std::string     m_netmask;
  std::string     m_peer_cn;   // CN from client certificate (empty = plain TCP)
  std::string     m_recv_buf; // reassembly buffer for partial frames
};

#endif // __openvpn_peer_hpp__
