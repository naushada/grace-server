#ifndef __main_app_cpp__
#define __main_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"
#include "fs_app.hpp"
#include "openvpn_client.hpp"
#include "openvpn_server.hpp"
#include "server_app.hpp"
#include "tls_config.hpp"

#include <cstdint>
#include <cstring>
#include <iostream>
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
    << "  Note: TUN interface name chosen by the kernel (next free tunX).\n";
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, const char *argv[]) {

  const std::string mode = get_flag(argc, argv, "mode", "server");

  if (mode != "server" && mode != "client") {
    std::cerr << "[main] unknown mode '" << mode << "'\n";
    print_usage(argv[0]);
    return 1;
  }

  // Shared across both modes.
  const tls_config tls{
    get_flag(argc, argv, "tls", "false") == "true",
    get_flag(argc, argv, "cert", ""),
    get_flag(argc, argv, "key",  ""),
    get_flag(argc, argv, "ca",   ""),
  };
  fs_app fs_mon("/app/command");

  if (mode == "client") {
    const std::string server      = get_flag(argc, argv, "server", "127.0.0.1");
    const uint16_t    port        = get_port_flag(argc, argv, "port", 1194);
    const std::string status_file = get_flag(argc, argv, "status", "/run/vpn_status.lua");

    std::cout << "[main] mode=client server=" << server << " port=" << port
              << " tls=" << (tls.enabled ? "ON" : "OFF") << '\n';

    openvpn_client vpn_client(server, port, status_file, tls);

  } else {
    const std::string pool_start = get_flag(argc, argv, "pool-start", "10.8.0.2");
    const std::string pool_end   = get_flag(argc, argv, "pool-end",   "10.8.0.254");
    const std::string server_ip  = get_flag(argc, argv, "server-ip",  "10.8.0.1");
    const std::string netmask    = get_flag(argc, argv, "netmask",    "255.255.255.0");

    std::cout << "[main] mode=server tls=" << (tls.enabled ? "ON" : "OFF")
              << " pool=" << pool_start << "–" << pool_end << '\n';

    server svc_module("0.0.0.0", 58989);
    openvpn_server vpn("0.0.0.0", 1194, pool_start, pool_end, tls, server_ip, netmask);
  }

  run_evt_loop{}();
  return 0;
}

#endif // __main_app_cpp__
