// vpn_client — standalone VPN tunnel client.
//
// Flags:
//   --server=<host>          VPN server address        (default: 127.0.0.1)
//   --port=<port>            VPN server port           (default: 1194)
//   --status=<path>          Lua status file           (default: /run/vpn_status.lua)
//   --tls=true               Enable TLS on VPN tunnel
//   --cert=<path>            PEM client certificate
//   --key=<path>             PEM private key
//   --ca=<path>              PEM CA certificate
//   --gnmi-port=<port>       gNMI server listen port   (default: 58989)
//   --gnmi-tls=true          Enable TLS on gNMI server
//   --gnmi-cert=<path>       PEM cert for gNMI TLS
//   --gnmi-key=<path>        PEM key for gNMI TLS
//   --gnmi-ca=<path>         PEM CA for gNMI TLS
//   --gnmi-probe=true        Fire a one-shot gNMI Get to the server VIP
//   --server-vip=<ip>        Server-side VPN IP to probe  (default: 10.8.0.1)
//
// Startup sequence (phased, no nested event loops):
//   Phase 1: event_base_loop(EVLOOP_ONCE) until VPN tunnel + tun0 are up.
//   Phase 2: start gNMI server on the assigned virtual IP.
//   Phase 3 (optional): blocking gNMI probe to the server VIP.
//   Phase 4: event_base_dispatch — keep tunnel alive indefinitely.

#include "openvpn_client.hpp"
#include "server_app.hpp"
#include "gnmi_client.hpp"
#include "tls_config.hpp"
#include "framework.hpp"
#include "gnmi/gnmi.pb.h"

#include <cstdint>
#include <iomanip>
#include <iostream>
#include <sstream>
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
    << "  --server=<host>          VPN server address        (default: 127.0.0.1)\n"
    << "  --port=<port>            VPN server port           (default: 1194)\n"
    << "  --status=<path>          Lua status file           (default: /run/vpn_status.lua)\n"
    << "  --tls=true               Enable TLS on VPN tunnel\n"
    << "  --cert=<path>            PEM client certificate\n"
    << "  --key=<path>             PEM private key\n"
    << "  --ca=<path>              PEM CA certificate\n"
    << "  --gnmi-port=<port>       gNMI server listen port   (default: 58989)\n"
    << "  --gnmi-tls=true          Enable TLS on gNMI server\n"
    << "  --gnmi-cert=<path>       PEM cert for gNMI TLS\n"
    << "  --gnmi-key=<path>        PEM key for gNMI TLS\n"
    << "  --gnmi-ca=<path>         PEM CA for gNMI TLS\n"
    << "  --gnmi-probe=true        Fire a one-shot gNMI Get to the server VIP\n"
    << "  --server-vip=<ip>        Server-side VPN IP to probe  (default: 10.8.0.1)\n";
}

static void dump_gnmi_response(const gnmi_client::response &r) {
  std::cout << "[gnmi-probe] grpc_status=" << r.grpc_status;
  if (!r.grpc_message.empty())
    std::cout << " message=\"" << r.grpc_message << '"';

  if (r.grpc_status == 0 && !r.body_pb.empty()) {
    gnmi::GetResponse resp;
    if (resp.ParseFromString(r.body_pb)) {
      std::cout << " notification_count=" << resp.notification_size();
      for (int i = 0; i < resp.notification_size(); ++i) {
        const auto &n = resp.notification(i);
        std::cout << "\n  [" << i << "] timestamp=" << n.timestamp()
                  << " update_count=" << n.update_size();
      }
    } else {
      std::ostringstream hex;
      hex << std::hex << std::setfill('0');
      for (unsigned char c : r.body_pb)
        hex << std::setw(2) << static_cast<int>(c);
      std::cout << " body_hex=" << hex.str();
    }
  }
  std::cout << '\n';
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

  // gNMI server TLS — used when the VPN server pushes a gNMI request back
  // through the tunnel to this client's gNMI server.
  const tls_config gnmi_tls{
    get_flag(argc, argv, "gnmi-tls", "false") == "true",
    get_flag(argc, argv, "gnmi-cert", ""),
    get_flag(argc, argv, "gnmi-key",  ""),
    get_flag(argc, argv, "gnmi-ca",   ""),
  };

  const std::string vpn_server  = get_flag(argc, argv, "server", "127.0.0.1");
  const uint16_t    port        = get_port(argc, argv, "port", 1194);
  const std::string status_file = get_flag(argc, argv, "status", "/run/vpn_status.lua");
  const uint16_t    gnmi_port   = get_port(argc, argv, "gnmi-port", 58989);
  const bool        gnmi_probe  = get_flag(argc, argv, "gnmi-probe", "false") == "true";
  const std::string server_vip  = get_flag(argc, argv, "server-vip", "10.8.0.1");

  std::cout << "[vpn_client] server=" << vpn_server << ":" << port
            << " tls=" << (tls.enabled ? "ON" : "OFF")
            << " gnmi-port=" << gnmi_port
            << " gnmi-tls=" << (gnmi_tls.enabled ? "ON" : "OFF")
            << (gnmi_probe ? " gnmi-probe=ON" : "") << '\n';

  // Phase 1: connect to VPN server and wait for tun0 + IP assignment.
  openvpn_client vpn(vpn_server, port, status_file, tls);

  std::cout << "[vpn_client] waiting for VPN tunnel...\n";
  auto *base = evt_base::instance().get();
  while (!vpn.ip_assigned())
    event_base_loop(base, EVLOOP_ONCE);

  std::cout << "[vpn_client] tunnel up, assigned=" << vpn.assigned_ip()
            << " — starting gNMI server on port " << gnmi_port
            << " tls=" << (gnmi_tls.enabled ? "ON" : "OFF") << '\n';

  // Phase 2: start gNMI server — now reachable via tun0 by the VPN server.
  server gnmi_svc("0.0.0.0", gnmi_port, gnmi_tls);

  // Phase 3 (optional): one-shot gNMI probe to the server VIP.
  if (gnmi_probe) {
    std::cout << "[vpn_client] probing " << server_vip << ":" << gnmi_port << "\n";

    gnmi::GetRequest req;
    req.mutable_prefix()->set_target("VIEWER");
    req.add_path()->add_elem()->set_name("interfaces");
    req.set_encoding(gnmi::JSON);

    std::string req_pb;
    req.SerializeToString(&req_pb);

    const auto resp = gnmi_client::call(server_vip, gnmi_port,
                                        "/gnmi.gNMI/Get", req_pb, gnmi_tls);
    dump_gnmi_response(resp);
    std::cout << "[vpn_client] probe done, tunnel remains open\n";
  }

  // Phase 4: keep the tunnel alive.
  run_evt_loop{}();
  return 0;
}
