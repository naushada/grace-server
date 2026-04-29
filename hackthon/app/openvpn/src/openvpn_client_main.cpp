// openvpn_client — wraps the system openvpn binary as a VPN client.
//
// Responsibility: spawn openvpn --client, detect tunnel-up and assigned VIP,
// then install iptables DNAT rules so traffic arriving at the VIP on the
// forwarded ports is transparently redirected to local services.
//
// Flags:
//   --server=<host>          OpenVPN server address         (default: 127.0.0.1)
//   --port=<port>            OpenVPN server port            (default: 1194)
//   --status=<path>          Lua status file                (default: /run/openvpn_status.lua)
//   --tls=true               Enable TLS (pass cert/key/ca to openvpn)
//   --cert=<path>            PEM client certificate
//   --key=<path>             PEM private key
//   --ca=<path>              PEM CA certificate
//   --fwd-host=<ip>          DNAT destination for all forwarded ports (default: 127.0.0.1)
//   --fwd-ports=<p1,p2,...>  Comma-separated ports to DNAT           (default: 80,443,58989)
//
// After construction the main loop runs indefinitely keeping the tunnel alive.
// iptables rules are removed automatically on exit.

#include "openvpn_client.hpp"
#include "tls_config.hpp"
#include "framework.hpp"

#include <cstdint>
#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

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

static std::vector<uint16_t> parse_ports(const std::string &s) {
  std::vector<uint16_t> ports;
  std::istringstream ss(s);
  std::string tok;
  while (std::getline(ss, tok, ',')) {
    if (!tok.empty())
      ports.push_back(static_cast<uint16_t>(std::stoi(tok)));
  }
  return ports;
}

static void print_usage(const char *prog) {
  std::cerr
    << "Usage: " << prog << " [options]\n"
    << "  --server=<host>          OpenVPN server address         (default: 127.0.0.1)\n"
    << "  --port=<port>            OpenVPN server port            (default: 1194)\n"
    << "  --status=<path>          Lua status file                (default: /run/openvpn_status.lua)\n"
    << "  --tls=true               Enable TLS (pass cert/key/ca to openvpn)\n"
    << "  --cert=<path>            PEM client certificate\n"
    << "  --key=<path>             PEM private key\n"
    << "  --ca=<path>              PEM CA certificate\n"
    << "  --fwd-host=<ip>          DNAT destination for forwarded ports (default: 127.0.0.1)\n"
    << "  --fwd-ports=<p1,p2,...>  Comma-separated ports to DNAT       (default: 80,443,58989)\n";
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

  const std::string vpn_server  = get_flag(argc, argv, "server", "127.0.0.1");
  const uint16_t    port        = get_port(argc, argv, "port", 1194);
  const std::string status_file = get_flag(argc, argv, "status", "/run/openvpn_status.lua");
  const std::string fwd_host    = get_flag(argc, argv, "fwd-host", "127.0.0.1");
  const std::string ports_str   = get_flag(argc, argv, "fwd-ports", "80,443,58989");

  const auto fwd_ports = parse_ports(ports_str);

  std::cout << "[openvpn_client] server=" << vpn_server << ":" << port
            << " tls=" << (tls.enabled ? "ON" : "OFF")
            << " fwd-host=" << fwd_host
            << " fwd-ports=" << ports_str << '\n';

  openvpn_client client(vpn_server, port, tls, status_file, fwd_ports, fwd_host);

  // Wait for tunnel to come up (openvpn logs "Initialization Sequence Completed").
  auto *base = evt_base::instance().get();
  std::cout << "[openvpn_client] waiting for tunnel...\n";
  while (!client.tunnel_up())
    event_base_loop(base, EVLOOP_ONCE);

  std::cout << "[openvpn_client] tunnel up, assigned=" << client.assigned_ip()
            << " — DNAT rules installed for ports " << ports_str
            << " \xe2\x86\x92 " << fwd_host << '\n';

  run_evt_loop{}();
  return 0;
}
