#ifndef __main_app_cpp__
#define __main_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"
#include "fs_app.hpp"
#include "gnmi_client.hpp"
#include "openvpn_client.hpp"
#include "openvpn_server.hpp"
#include "server_app.hpp"
#include "tls_config.hpp"

#include "gnmi/gnmi.pb.h"

#include <iomanip>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Argument parsing helpers
// ---------------------------------------------------------------------------

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

static uint16_t get_port_flag(int argc, const char *argv[],
                                const std::string &name, uint16_t def) {
  const auto s = get_flag(argc, argv, name, "");
  return s.empty() ? def : static_cast<uint16_t>(std::stoi(s));
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

static void print_usage(const char *prog) {
  std::cerr
    << "Usage:\n"
    << "  " << prog << " [--mode=server]                          (default)\n"
    << "  " << prog << " --mode=server\n"
    << "       Starts the gNMI gRPC server (port 58989) and the OpenVPN\n"
    << "       TCP tunnel server (port 1194) with a 10.8.0.2-254 IP pool.\n"
    << "\n"
    << "  " << prog << " --mode=client [options]\n"
    << "       Connects to an openvpn_server, receives a virtual IP,\n"
    << "       creates a TUN interface, and writes connection state to a\n"
    << "       Lua status file.\n"
    << "\n"
    << "  Client options:\n"
    << "    --server=<host>          VPN server address  (default: 127.0.0.1)\n"
    << "    --port=<port>            VPN server port     (default: 1194)\n"
    << "    --status=<path>          Lua status file     (default: /run/vpn_status.lua)\n"
    << "    --tls=true               Enable TLS (default: false)\n"
    << "    --cert=<path>            PEM certificate file\n"
    << "    --key=<path>             PEM private key file\n"
    << "    --ca=<path>              PEM CA certificate (peer verification)\n"
    << "    --gnmi-probe=true        After VPN connects, fire a gNMI Get through\n"
    << "                             the tunnel and dump the response; tunnel stays open.\n"
    << "    --server-vip=<ip>        Server-side VPN IP to probe  (default: 10.8.0.1)\n"
    << "    --gnmi-port=<port>       gNMI port on the server      (default: 58989)\n"
    << "    --gnmi-tls=true          Use TLS for the gNMI probe connection\n"
    << "    --gnmi-cert=<path>       PEM client cert for gNMI probe TLS\n"
    << "    --gnmi-key=<path>        PEM private key for gNMI probe TLS\n"
    << "    --gnmi-ca=<path>         PEM CA cert for gNMI probe TLS\n"
    << "  Note: TUN interface name chosen by the kernel (next free tunX).\n"
    << "\n"
    << "  Server options:\n"
    << "    --gnmi-tls=true          Enable TLS on the gNMI listener (port 58989)\n"
    << "    --gnmi-cert=<path>       PEM server certificate for gNMI TLS\n"
    << "    --gnmi-key=<path>        PEM private key for gNMI TLS\n"
    << "    --gnmi-ca=<path>         PEM CA cert (enables mTLS client auth)\n";
}

// ---------------------------------------------------------------------------
// gNMI probe — fires after the VPN tunnel is up.
//
// Design: phased event loop (sequential, NOT nested):
//
//   Phase 1  while (!vpn.ip_assigned()) event_base_loop(EVLOOP_ONCE)
//            ↳ drives the openvpn_client bufferevent until IP_ASSIGN arrives
//              and tun0 is configured
//
//   Phase 2  gnmi_client::call(server_vip, gnmi_port, "/gnmi.gNMI/Get", req)
//            ↳ internally loops event_base_loop(EVLOOP_ONCE) until response
//              arrives — safe because Phase 1 has already returned, no nesting
//
//   Phase 3  event_base_dispatch(base)
//            ↳ keeps the tunnel alive; tun I/O and reconnect logic continue
//
// event_base_loop(EVLOOP_ONCE) is NOT re-entrant — calling it from within an
// already-running dispatch would be undefined behaviour.  The phased approach
// avoids that by never calling it from inside an event callback.
// ---------------------------------------------------------------------------

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
      // Fallback: hex dump of raw proto bytes
      std::ostringstream hex;
      hex << std::hex << std::setfill('0');
      for (unsigned char c : r.body_pb)
        hex << std::setw(2) << static_cast<int>(c);
      std::cout << " body_hex=" << hex.str();
    }
  }
  std::cout << '\n';
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, const char *argv[]) {
  // Docker pipes stdout through a non-TTY, making it block-buffered by default.
  // Force unit (per-write) flushing so log lines appear immediately.
  std::cout << std::unitbuf;

  const std::string mode = get_flag(argc, argv, "mode", "server");

  if (mode != "server" && mode != "client") {
    std::cerr << "[main] unknown mode '" << mode << "'\n";
    print_usage(argv[0]);
    return 1;
  }

  // VPN TLS — shared across both modes.
  const tls_config tls{
    get_flag(argc, argv, "tls", "false") == "true",
    get_flag(argc, argv, "cert", ""),
    get_flag(argc, argv, "key",  ""),
    get_flag(argc, argv, "ca",   ""),
  };
  // gNMI TLS — independent from VPN TLS.
  const tls_config gnmi_tls{
    get_flag(argc, argv, "gnmi-tls", "false") == "true",
    get_flag(argc, argv, "gnmi-cert", ""),
    get_flag(argc, argv, "gnmi-key",  ""),
    get_flag(argc, argv, "gnmi-ca",   ""),
  };
  fs_app fs_mon("/app/command");

  if (mode == "client") {
    const std::string server      = get_flag(argc, argv, "server", "127.0.0.1");
    const uint16_t    port        = get_port_flag(argc, argv, "port", 1194);
    const std::string status_file = get_flag(argc, argv, "status", "/run/vpn_status.lua");
    const bool        gnmi_probe  = get_flag(argc, argv, "gnmi-probe", "false") == "true";
    const std::string server_vip  = get_flag(argc, argv, "server-vip", "10.8.0.1");
    const uint16_t    gnmi_port   = get_port_flag(argc, argv, "gnmi-port", 58989);

    std::cout << "[main] mode=client server=" << server << " port=" << port
              << " tls=" << (tls.enabled ? "ON" : "OFF")
              << (gnmi_probe ? " gnmi-probe=ON" : "") << '\n';

    // vpn_client + event loop must stay in the same scope so the bufferevent
    // is alive for the entire duration of run_evt_loop.
    openvpn_client vpn_client(server, port, status_file, tls);

    if (gnmi_probe) {
      std::cout << "[main] waiting for VPN tunnel...\n";
      auto *base = evt_base::instance().get();
      while (!vpn_client.ip_assigned())
        event_base_loop(base, EVLOOP_ONCE);

      std::cout << "[main] tunnel up, assigned=" << vpn_client.assigned_ip()
                << " probing " << server_vip << ":" << gnmi_port << "\n";

      gnmi::GetRequest req;
      req.mutable_prefix()->set_target("VIEWER");
      auto *path = req.add_path();
      path->add_elem()->set_name("interfaces");
      req.set_encoding(gnmi::JSON);

      std::string req_pb;
      req.SerializeToString(&req_pb);

      const auto resp = gnmi_client::call(server_vip, gnmi_port,
                                           "/gnmi.gNMI/Get", req_pb, gnmi_tls);
      dump_gnmi_response(resp);
      std::cout << "[main] probe done, tunnel remains open\n";
    }

    run_evt_loop{}();

  } else {
    const std::string pool_start = get_flag(argc, argv, "pool-start", "10.8.0.2");
    const std::string pool_end   = get_flag(argc, argv, "pool-end",   "10.8.0.254");
    const std::string server_ip  = get_flag(argc, argv, "server-ip",  "10.8.0.1");
    const std::string netmask    = get_flag(argc, argv, "netmask",    "255.255.255.0");

    std::cout << "[main] mode=server tls=" << (tls.enabled ? "ON" : "OFF")
              << " gnmi-tls=" << (gnmi_tls.enabled ? "ON" : "OFF")
              << " pool=" << pool_start << "–" << pool_end << '\n';

    // svc_module + vpn must stay in scope for the entire event loop.
    server svc_module("0.0.0.0", 58989, gnmi_tls);
    openvpn_server vpn("0.0.0.0", 1194, pool_start, pool_end, tls, server_ip, netmask);

    run_evt_loop{}();
  }

  return 0;
}

#endif // __main_app_cpp__
