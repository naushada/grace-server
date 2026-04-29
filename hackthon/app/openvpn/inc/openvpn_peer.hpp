#ifndef __openvpn_peer_hpp__
#define __openvpn_peer_hpp__

#include "framework.hpp"
#include "mqtt_io.hpp"
#include "vpn_types.hpp"

#include <cstdint>
#include <memory>
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
// ═══════════════════════════════════════════════════════════════════════════
// MQTT / gNMI forwarding
// ═══════════════════════════════════════════════════════════════════════════
//
// When the MQTT broker config is provided at construction, each peer creates
// its own mqtt_io connection and subscribes to "fwd/<assigned_vip>".
//
// Incoming MQTT message flow:
//   CLI publishes to "cli/<vip>"
//   → gnmi-client-svc relays to "fwd/<vip>"
//   → openvpn_peer receives, parses rpc_path '\0' proto_bytes
//   → gnmi_client::push_async(vip, gnmi_port, rpc_path, proto_bytes)
//   → response published to "resp/<vip>"
//   → gnmi-client-svc relays to "cli_resp/<vip>"
//   → CLI receives the response
class openvpn_peer : public evt_io {
public:
  static constexpr uint8_t TYPE_IP_ASSIGN  = 0x01;
  static constexpr uint8_t TYPE_DATA       = 0x02;
  static constexpr uint8_t TYPE_DISCONNECT = 0x03;
  static constexpr size_t  HEADER_LEN      = 5;

  // Plain TCP peer (pre-accepted fd).
  openvpn_peer(int32_t channel, const std::string &peer_host,
               openvpn_server *parent, const std::string &assigned_ip,
               const std::string &netmask, const mqtt_sub_cfg &mqtt = {});

  // TLS or pre-wrapped bev peer.
  openvpn_peer(struct bufferevent *bev, const std::string &peer_host,
               openvpn_server *parent, const std::string &assigned_ip,
               const std::string &netmask, const mqtt_sub_cfg &mqtt = {});

  virtual ~openvpn_peer() = default;

  const std::string &assigned_ip() const { return m_assigned_ip; }
  void forward_data(const std::string &pkt) { send_frame(TYPE_DATA, pkt); }

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
  void   setup_mqtt(const mqtt_sub_cfg &cfg);

  static std::string extract_cn(struct bufferevent *bev);

  // MQTT message callback — one instance per peer, userdata = this peer.
  static void on_mqtt_message(struct mosquitto *, void *userdata,
                               const struct mosquitto_message *msg);

  openvpn_server          *m_parent;
  std::string              m_assigned_ip;
  std::string              m_netmask;
  std::string              m_peer_cn;
  std::string              m_recv_buf;
  std::unique_ptr<mqtt_io> m_mqtt_io;
  uint16_t                 m_gnmi_port{58989};
};

#endif // __openvpn_peer_hpp__
