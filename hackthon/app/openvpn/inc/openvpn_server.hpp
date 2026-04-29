#ifndef __openvpn_server_hpp__
#define __openvpn_server_hpp__

#include "framework.hpp"
#include "mqtt_io.hpp"
#include "tls_config.hpp"
#include "vpn_types.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>

// openvpn_server — wraps the standard `openvpn --server` binary.
//
// Spawns the system openvpn binary with the management interface enabled
// (--management 127.0.0.1 <mgmt_port>).  Once openvpn is running, the
// mgmt_io inner class connects to the management interface using a plain
// TCP evt_io outbound connection (retries automatically until openvpn is
// ready).
//
// Client tracking via status polling:
//   A 5-second repeating timer sends "status 2\n" to the management socket.
//   The ROUTING TABLE section of the response lists currently connected
//   client VIPs.  On each poll the new set is diffed against the previous:
//     - new VIP  → on_client_connect(vip): subscribe to MQTT fwd/<vip>
//     - gone VIP → on_client_disconnect(vip): unsubscribe
//
// gNMI forwarding (when mqtt is enabled):
//   Each connected client gets its own mqtt_io subscriber on fwd/<vip>.
//   Incoming MQTT payloads (rpc_path '\0' proto_bytes) are forwarded to the
//   client's gNMI server at <vip>:<mqtt.gnmi_port> via gnmi_client::push_async.
//   Responses are published on resp/<vip> for gnmi-client-svc to relay back.
class openvpn_server {
public:
  openvpn_server(uint16_t            vpn_port,
                 const tls_config   &tls       = {},
                 uint16_t            mgmt_port = 7505,
                 const mqtt_sub_cfg &mqtt      = {});
  ~openvpn_server();

private:
  class proc_io;
  class mgmt_io;
  friend class proc_io;
  friend class mgmt_io;

  void on_client_connect(const std::string &vip);
  void on_client_disconnect(const std::string &vip);

  // MQTT callback: forwards gNMI requests from CLI to the VPN client.
  static void on_mqtt_message(struct mosquitto *, void *userdata,
                               const struct mosquitto_message *msg);

  pid_t        m_pid{-1};
  int          m_pipe_r{-1};
  uint16_t     m_mgmt_port;
  mqtt_sub_cfg m_mqtt;

  // Status-poll line accumulator and current active client set.
  std::string              m_mgmt_buf;
  bool                     m_in_routing_table{false};
  std::unordered_set<std::string> m_active_vips;

  // Per-VIP MQTT subscriber — one per connected client.
  std::unordered_map<std::string, std::unique_ptr<mqtt_io>> m_peers;

  std::unique_ptr<proc_io> m_proc_io;
  std::unique_ptr<mgmt_io> m_mgmt_io;
};

#endif // __openvpn_server_hpp__
