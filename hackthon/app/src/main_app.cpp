#ifndef __main_app_cpp__
#define __main_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"
#include "fs_app.hpp"
#include "openvpn_server.hpp"
#include "server_app.hpp"

int main(std::int32_t argc, const char *argv[]) {

  fs_app fs_mon("/app/command");

  // gNMI server — accepts gRPC connections from CLI and peer devices.
  server svc_module("0.0.0.0", 58989);
  std::cout << "gNMI server started on port:58989\n";

  // VPN tunnel server — CLI update commands connect here first to receive
  // a virtual IP from the pool before forwarding the gNMI Set request.
  openvpn_server vpn("0.0.0.0", 1194);

  run_evt_loop main_loop;
  main_loop();

  return 0;
}
#endif //__main_app_cpp__
