#ifndef __vpn_types_hpp__
#define __vpn_types_hpp__

#include <cstdint>
#include <string>

// MQTT broker connection details shared by vpn_server (stores and passes
// to peers) and vpn_peer (creates its own per-connection subscription).
struct mqtt_sub_cfg {
  bool        enabled{false};
  std::string host{"localhost"};
  uint16_t    port{1883};
  uint16_t    gnmi_port{58989};  // port to connect to on the client's gNMI server
};

#endif // __vpn_types_hpp__
