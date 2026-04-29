// vpn_client — standalone VPN tunnel client.
//
// Responsibility: VPN tunnel management + iptables NAT forwarding only.
// The gNMI server is always a separate service (gnmi-server-svc); this
// binary never starts an embedded gNMI server.
//
// Flags:
//   --server=<host>          VPN server address        (default: 127.0.0.1)
//   --port=<port>            VPN server port           (default: 1194)
//   --status=<path>          Lua status file           (default: /run/vpn_status.lua)
//   --tls=true               Enable TLS on VPN tunnel
//   --cert=<path>            PEM client certificate
//   --key=<path>             PEM private key
//   --ca=<path>              PEM CA certificate
//   --gnmi-port=<port>       gNMI port on the assigned VIP  (default: 58989)
//   --gnmi-fwd-ip=<ip>       Forward VIP:<gnmi-port> → this IP (gnmi-server-svc)
//   --gnmi-fwd-port=<port>   Target port for the DNAT rule    (default: gnmi-port)
//   --gnmi-probe=true        Fire a one-shot gNMI Get to the server VIP (diagnostic)
//   --server-vip=<ip>        Server-side VPN IP to probe  (default: 10.8.0.1)
//
// Startup sequence (phased, no nested event loops):
//   Phase 1: event_base_loop(EVLOOP_ONCE) until VPN tunnel + tun0 are up.
//            openvpn_client installs iptables PREROUTING DNAT for the VIP.
//   Phase 2 (optional): blocking gNMI probe to the server VIP.
//   Phase 3: event_base_dispatch — keep tunnel alive indefinitely.

#include "openvpn_client.hpp"
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
    << "  --gnmi-port=<port>       gNMI port on the assigned VIP (default: 58989)\n"
    << "  --gnmi-fwd-ip=<ip>       DNAT destination IP (gnmi-server-svc address)\n"
    << "  --gnmi-fwd-port=<port>   DNAT destination port         (default: gnmi-port)\n"
    << "  --gnmi-probe=true        One-shot gNMI Get to server VIP (diagnostic)\n"
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

  const tls_config tls{
    get_flag(argc, argv, "tls", "false") == "true",
    get_flag(argc, argv, "cert", ""),
    get_flag(argc, argv, "key",  ""),
    get_flag(argc, argv, "ca",   ""),
  };

  const std::string vpn_server    = get_flag(argc, argv, "server", "127.0.0.1");
  const uint16_t    port          = get_port(argc, argv, "port", 1194);
  const std::string status_file   = get_flag(argc, argv, "status", "/run/vpn_status.lua");
  const uint16_t    gnmi_port     = get_port(argc, argv, "gnmi-port", 58989);
  const std::string gnmi_fwd_ip   = get_flag(argc, argv, "gnmi-fwd-ip", "");
  const uint16_t    gnmi_fwd_port = get_port(argc, argv, "gnmi-fwd-port", gnmi_port);
  const bool        gnmi_probe    = get_flag(argc, argv, "gnmi-probe", "false") == "true";
  const std::string server_vip    = get_flag(argc, argv, "server-vip", "10.8.0.1");

  if (gnmi_fwd_ip.empty()) {
    std::cerr << "[vpn_client] --gnmi-fwd-ip is required (gnmi-server-svc address)\n";
    print_usage(argv[0]);
    return 1;
  }

  std::cout << "[vpn_client] server=" << vpn_server << ":" << port
            << " tls=" << (tls.enabled ? "ON" : "OFF")
            << " gnmi-port=" << gnmi_port
            << " gnmi-fwd=" << gnmi_fwd_ip << ":" << gnmi_fwd_port
            << (gnmi_probe ? " gnmi-probe=ON" : "") << '\n';

  // Phase 1: connect and wait for IP assignment.
  // openvpn_client installs the iptables DNAT rule as soon as the VIP is known.
  openvpn_client vpn(vpn_server, port, status_file, tls,
                     gnmi_port, gnmi_fwd_ip, gnmi_fwd_port);

  std::cout << "[vpn_client] waiting for VPN tunnel...\n";
  auto *base = evt_base::instance().get();
  while (!vpn.ip_assigned())
    event_base_loop(base, EVLOOP_ONCE);

  std::cout << "[vpn_client] tunnel up, assigned=" << vpn.assigned_ip()
            << " — DNAT " << vpn.assigned_ip() << ":" << gnmi_port
            << " \xe2\x86\x92 " << gnmi_fwd_ip << ":" << gnmi_fwd_port << '\n';

  // Phase 2 (optional): one-shot gNMI diagnostic probe to the server VIP.
  if (gnmi_probe) {
    std::cout << "[vpn_client] probing " << server_vip << ":" << gnmi_port << "\n";

    gnmi::GetRequest req;
    req.mutable_prefix()->set_target("VIEWER");
    req.add_path()->add_elem()->set_name("interfaces");
    req.set_encoding(gnmi::JSON);

    std::string req_pb;
    req.SerializeToString(&req_pb);

    const auto resp = gnmi_client::call(server_vip, gnmi_port,
                                        "/gnmi.gNMI/Get", req_pb);
    dump_gnmi_response(resp);
    std::cout << "[vpn_client] probe done, tunnel remains open\n";
  }

  // Phase 3: keep the tunnel alive.
  run_evt_loop{}();
  return 0;
}
