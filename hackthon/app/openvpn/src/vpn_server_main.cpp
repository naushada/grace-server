// vpn_server — standalone VPN tunnel server.
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
//   --gnmi-push=true         Push a gNMI Get to each client after tunnel up
//   --gnmi-port=<port>       Client gNMI port to push to   (default: 58989)
//   --gnmi-push-delay=<s>    Seconds before push            (default: 2)
//   --gnmi-tls=true          Enable TLS when pushing to client gNMI server
//   --gnmi-cert=<path>       PEM cert for gNMI TLS
//   --gnmi-key=<path>        PEM key for gNMI TLS
//   --gnmi-ca=<path>         PEM CA for gNMI TLS
//   --mqtt-host=<host>       MQTT broker — enables MQTT subscriber on fwd/#
//   --mqtt-port=<port>       MQTT broker port              (default: 1883)

#include "openvpn_server.hpp"
#include "server_app.hpp"
#include "gnmi_client.hpp"
#include "tls_config.hpp"
#include "framework.hpp"
#include "gnmi/gnmi.pb.h"

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
    << "  --gnmi-push=true         Push gNMI Get to each client after tunnel up\n"
    << "  --gnmi-port=<port>       Client gNMI port to push to  (default: 58989)\n"
    << "  --gnmi-push-delay=<s>    Seconds before push           (default: 2)\n"
    << "  --gnmi-tls=true          Enable TLS on gNMI push client\n"
    << "  --gnmi-cert=<path>       PEM cert for gNMI TLS\n"
    << "  --gnmi-key=<path>        PEM key for gNMI TLS\n"
    << "  --gnmi-ca=<path>         PEM CA for gNMI TLS\n"
    << "  --mqtt-host=<host>       MQTT broker (enables subscriber on fwd/#)\n"
    << "  --mqtt-port=<port>       MQTT broker port             (default: 1883)\n";
}

int main(int argc, const char *argv[]) {
  std::cout << std::unitbuf;

  if (get_flag(argc, argv, "help") == "true") {
    print_usage(argv[0]);
    return 0;
  }

  // VPN tunnel TLS.
  const tls_config tls{
    get_flag(argc, argv, "tls", "false") == "true",
    get_flag(argc, argv, "cert", ""),
    get_flag(argc, argv, "key",  ""),
    get_flag(argc, argv, "ca",   ""),
  };

  // gNMI push-client TLS (used when pushing a Get to connected VPN clients).
  const tls_config gnmi_tls{
    get_flag(argc, argv, "gnmi-tls", "false") == "true",
    get_flag(argc, argv, "gnmi-cert", ""),
    get_flag(argc, argv, "gnmi-key",  ""),
    get_flag(argc, argv, "gnmi-ca",   ""),
  };

  const std::string server_ip  = get_flag(argc, argv, "server-ip",  "10.8.0.1");
  const std::string pool_start = get_flag(argc, argv, "pool-start", "10.8.0.2");
  const std::string pool_end   = get_flag(argc, argv, "pool-end",   "10.8.0.254");
  const std::string netmask    = get_flag(argc, argv, "netmask",    "255.255.255.0");
  const uint16_t    port       = get_port(argc, argv, "port", 1194);

  // gNMI push configuration — server fires this at each newly-connected client.
  gnmi_push_cfg gnmi_push;
  gnmi_push.enabled  = get_flag(argc, argv, "gnmi-push", "false") == "true";
  gnmi_push.port     = get_port(argc, argv, "gnmi-port", 58989);
  gnmi_push.tls      = gnmi_tls;
  gnmi_push.delay_s  = std::stoi(get_flag(argc, argv, "gnmi-push-delay", "2"));
  if (gnmi_push.enabled) {
    gnmi::GetRequest req;
    req.mutable_prefix()->set_target("VIEWER");
    req.add_path()->add_elem()->set_name("interfaces");
    req.set_encoding(gnmi::JSON);
    req.SerializeToString(&gnmi_push.request_pb);
  }

  // MQTT subscriber — receives gNMI requests from gnmi-client-svc and injects
  // them through the VPN tunnel to the addressed client.
  mqtt_sub_cfg mqtt_sub;
  mqtt_sub.enabled   = !get_flag(argc, argv, "mqtt-host", "").empty();
  mqtt_sub.host      = get_flag(argc, argv, "mqtt-host", "localhost");
  mqtt_sub.port      = get_port(argc, argv, "mqtt-port", 1883);
  mqtt_sub.gnmi_port = get_port(argc, argv, "gnmi-port", 58989);

  std::cout << "[vpn_server] port=" << port
            << " tls=" << (tls.enabled ? "ON" : "OFF")
            << " gnmi-tls=" << (gnmi_tls.enabled ? "ON" : "OFF")
            << " gnmi-push=" << (gnmi_push.enabled ? "ON" : "OFF")
            << " mqtt=" << (mqtt_sub.enabled ? "ON" : "OFF")
            << " pool=" << pool_start << "\xe2\x80\x93" << pool_end << '\n';

  // gNMI server on this host — reachable by VPN clients through the tunnel.
  server gnmi_svc("0.0.0.0", 58989, gnmi_tls);

  // VPN tunnel server — accepts clients, assigns IPs, routes frames.
  openvpn_server vpn("0.0.0.0", port, pool_start, pool_end, tls,
                     server_ip, netmask, gnmi_push, mqtt_sub);

  run_evt_loop{}();
  return 0;
}
