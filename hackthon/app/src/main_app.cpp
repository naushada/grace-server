#ifndef __main_app_cpp__
#define __main_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"
#include "fs_app.hpp"
#include "openvpn_client.hpp"
#include "openvpn_server.hpp"
#include "server_app.hpp"

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
    << "  Note: the TUN interface name is chosen by the kernel (next free tunX).\n";
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

  if (mode == "client") {
    // ------------------------------------------------------------------
    // Client mode — connect to tunnel server, get virtual IP, run loop.
    // ------------------------------------------------------------------
    const std::string server      = get_flag(argc, argv, "server", "127.0.0.1");
    const uint16_t    port        = get_port_flag(argc, argv, "port", 1194);
    const std::string status_file = get_flag(argc, argv, "status",
                                              "/run/vpn_status.lua");

    std::cout << "[main] mode=client server=" << server
              << " port=" << port
              << " status=" << status_file << "\n";
    std::cout << "[main] TUN interface will be assigned by the kernel\n";

    fs_app fs_mon("/app/command");

    // openvpn_client connects; kernel picks the tunX name after IP_ASSIGN.
    openvpn_client vpn_client(server, port, status_file);

    run_evt_loop main_loop;
    main_loop();

  } else {
    // ------------------------------------------------------------------
    // Server mode (default) — gNMI + OpenVPN server.
    // ------------------------------------------------------------------
    std::cout << "[main] mode=server\n";

    fs_app fs_mon("/app/command");

    // gNMI gRPC server — accepts connections from CLI and peer devices.
    server svc_module("0.0.0.0", 58989);
    std::cout << "[main] gNMI server on port 58989\n";

    // OpenVPN TCP tunnel server — CLI update commands connect here first
    // to receive a virtual IP before sending the gNMI Set request.
    openvpn_server vpn("0.0.0.0", 1194);

    run_evt_loop main_loop;
    main_loop();
  }

  return 0;
}

#endif // __main_app_cpp__
