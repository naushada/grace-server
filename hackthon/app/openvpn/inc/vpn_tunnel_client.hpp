#ifndef __vpn_tunnel_client_hpp__
#define __vpn_tunnel_client_hpp__

#include <cstdint>
#include <string>

// Client-side VPN tunnel connection.
//
// Connects to an vpn_server over TCP, waits for the IP_ASSIGN frame,
// and returns the assigned virtual address.  All I/O is driven through the
// shared evt_base event loop (EVLOOP_ONCE) — same pattern as gnmi_client.
//
// Usage (from readline.cpp update handlers):
//   auto tr = vpn_tunnel_client::connect(tunnel_host, tunnel_port);
//   if (!tr.ok) { ... handle error ... }
//   // tr.assigned_ip holds e.g. "10.8.0.3"
class vpn_tunnel_client {
public:
  struct result {
    bool        ok{false};
    std::string assigned_ip;
    std::string message;
  };

  static result connect(const std::string &host, uint16_t port);
};

#endif // __vpn_tunnel_client_hpp__
