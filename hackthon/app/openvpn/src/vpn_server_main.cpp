// vpn_server — standalone VPN tunnel server.
//
// When a client connects, the assigned openvpn_peer subscribes to the MQTT
// broker on "fwd/<client-vip>" and handles gNMI Get/Update/Replace/Delete
// requests forwarded from the CLI via gnmi-client-svc.  Responses are
// published back on "resp/<client-vip>" for the relay to return to the CLI.
//
// Flags:
//   --port=<port>            VPN listen port           (default: 1194)
//   --server-ip=<ip>         Server TUN IP             (default: 10.8.0.1)
//   --pool-start=<ip>        First client IP           (default: 10.8.0.2)
//   --pool-end=<ip>          Last client IP            (default: 10.8.0.254)
//   --netmask=<mask>         Tunnel netmask            (default: 255.255.255.0)
//   --tls=true               Enable TLS on VPN tunnel
//   --cert=<path>            PEM server certificate
//   --key=<path>             PEM private key
//   --ca=<path>              PEM CA certificate
//   --mqtt-host=<host>       MQTT broker — enables per-peer fwd/<vip> subscriber
//   --mqtt-port=<port>       MQTT broker port          (default: 1883)
//   --gnmi-port=<port>       Client gNMI port peers connect to (default: 58989)

#include "openvpn_server.hpp"
#include "server_app.hpp"
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
    << "  --port=<port>            VPN listen port           (default: 1194)\n"
    << "  --server-ip=<ip>         Server TUN IP             (default: 10.8.0.1)\n"
    << "  --pool-start=<ip>        First client IP           (default: 10.8.0.2)\n"
    << "  --pool-end=<ip>          Last client IP            (default: 10.8.0.254)\n"
    << "  --netmask=<mask>         Tunnel netmask            (default: 255.255.255.0)\n"
    << "  --tls=true               Enable TLS on VPN tunnel\n"
    << "  --cert=<path>            PEM server certificate\n"
    << "  --key=<path>             PEM private key\n"
    << "  --ca=<path>              PEM CA certificate\n"
    << "  --mqtt-host=<host>       MQTT broker — enables per-peer gNMI forwarding\n"
    << "  --mqtt-port=<port>       MQTT broker port          (default: 1883)\n"
    << "  --gnmi-port=<port>       Client gNMI port to forward to (default: 58989)\n";
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

  const std::string server_ip  = get_flag(argc, argv, "server-ip",  "10.8.0.1");
  const std::string pool_start = get_flag(argc, argv, "pool-start", "10.8.0.2");
  const std::string pool_end   = get_flag(argc, argv, "pool-end",   "10.8.0.254");
  const std::string netmask    = get_flag(argc, argv, "netmask",    "255.255.255.0");
  const uint16_t    port       = get_port(argc, argv, "port", 1194);

  // MQTT broker config — forwarded to each peer on connect.
  // Each peer subscribes to "fwd/<vip>" and handles incoming gNMI requests.
  mqtt_sub_cfg mqtt;
  mqtt.enabled   = !get_flag(argc, argv, "mqtt-host", "").empty();
  mqtt.host      = get_flag(argc, argv, "mqtt-host", "localhost");
  mqtt.port      = get_port(argc, argv, "mqtt-port", 1883);
  mqtt.gnmi_port = get_port(argc, argv, "gnmi-port", 58989);

  std::cout << "[vpn_server] port=" << port
            << " tls=" << (tls.enabled ? "ON" : "OFF")
            << " mqtt=" << (mqtt.enabled ? mqtt.host + ":" + std::to_string(mqtt.port) : "OFF")
            << " pool=" << pool_start << "\xe2\x80\x93" << pool_end << '\n';

  // gNMI server on this host — reachable by probe clients through the tunnel.
  server gnmi_svc("0.0.0.0", 58989);

  // VPN tunnel server — each accepted connection becomes an openvpn_peer.
  openvpn_server vpn("0.0.0.0", port, pool_start, pool_end, tls,
                     server_ip, netmask, mqtt);

  run_evt_loop{}();
  return 0;
}
