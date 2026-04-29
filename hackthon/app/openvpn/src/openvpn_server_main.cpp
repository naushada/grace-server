// openvpn_server — wraps the system openvpn binary as a VPN server.
//
// Responsibility: spawn openvpn --server, monitor connected clients via the
// management interface (status polling every 5 s), and forward gNMI requests
// arriving over MQTT (fwd/<client-vip>) to each client's gNMI server, then
// publish responses back on resp/<client-vip>.
//
// Flags:
//   --port=<port>            OpenVPN listen port            (default: 1194)
//   --mgmt-port=<port>       Management interface port      (default: 7505)
//   --tls=true               Enable TLS (pass cert/key/ca to openvpn)
//   --cert=<path>            PEM server certificate
//   --key=<path>             PEM private key
//   --ca=<path>              PEM CA certificate
//   --mqtt-host=<host>       MQTT broker — enables per-client gNMI forwarding
//   --mqtt-port=<port>       MQTT broker port               (default: 1883)
//   --gnmi-port=<port>       Client-side gNMI port to forward to (default: 58989)

#include "openvpn_server.hpp"
#include "tls_config.hpp"
#include "vpn_types.hpp"
#include "framework.hpp"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

static std::string get_flag(int argc, const char *argv[],
                             const std::string &name,
                             const std::string &def = "") {
  const std::string prefix = "--" + name + "=";
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg.starts_with(prefix))
      return std::string(arg.substr(prefix.size()));
  }
  return def;
}

static uint16_t get_port(int argc, const char *argv[],
                          const std::string &name, uint16_t def) {
  const auto s = get_flag(argc, argv, name, "");
  return s.empty() ? def : static_cast<uint16_t>(std::stoi(s));
}

static void print_usage(const char *prog) {
  std::cerr
    << "Usage: " << prog << " [options]\n"
    << "  --port=<port>            OpenVPN listen port            (default: 1194)\n"
    << "  --mgmt-port=<port>       Management interface port      (default: 7505)\n"
    << "  --tls=true               Enable TLS (pass cert/key/ca to openvpn)\n"
    << "  --cert=<path>            PEM server certificate\n"
    << "  --key=<path>             PEM private key\n"
    << "  --ca=<path>              PEM CA certificate\n"
    << "  --mqtt-host=<host>       MQTT broker — enables per-client gNMI forwarding\n"
    << "  --mqtt-port=<port>       MQTT broker port               (default: 1883)\n"
    << "  --gnmi-port=<port>       Client-side gNMI port          (default: 58989)\n";
}

int main(int argc, const char *argv[]) {
  std::cout << std::unitbuf;

  if (get_flag(argc, argv, "help") == "true") {
    print_usage(argv[0]);
    return 0;
  }

  const tls_config tls{
    get_flag(argc, argv, "tls", "false") == "true",
    get_flag(argc, argv, "cert", ""),
    get_flag(argc, argv, "key",  ""),
    get_flag(argc, argv, "ca",   ""),
  };

  const uint16_t port      = get_port(argc, argv, "port",      1194);
  const uint16_t mgmt_port = get_port(argc, argv, "mgmt-port", 7505);

  mqtt_sub_cfg mqtt;
  mqtt.enabled   = !get_flag(argc, argv, "mqtt-host", "").empty();
  mqtt.host      = get_flag(argc, argv, "mqtt-host", "localhost");
  mqtt.port      = get_port(argc, argv, "mqtt-port", 1883);
  mqtt.gnmi_port = get_port(argc, argv, "gnmi-port", 58989);

  std::cout << "[openvpn_server] port=" << port
            << " mgmt-port=" << mgmt_port
            << " tls=" << (tls.enabled ? "ON" : "OFF")
            << " mqtt=" << (mqtt.enabled
                             ? mqtt.host + ":" + std::to_string(mqtt.port)
                             : "OFF")
            << '\n';

  openvpn_server server(port, tls, mgmt_port, mqtt);

  run_evt_loop{}();
  return 0;
}
