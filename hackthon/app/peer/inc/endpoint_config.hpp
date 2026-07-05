#ifndef __endpoint_config_hpp__
#define __endpoint_config_hpp__

// Loads the gnmi_peer endpoint configuration from a Lua file.
//
// Expected schema (both endpoint forms are accepted):
//
//   return {
//     ["local"]  = { endpoint = { ip = "0.0.0.0",   port = 58989 } },
//     ["remote"] = { endpoint = "peer.example.com:58990" },
//     tls = { enabled = false, cert = "", key = "", ca = "" },
//   }
//
// `local`/`remote` are Lua keywords, so they MUST be written as the quoted
// table keys ["local"] / ["remote"]. Each `endpoint` is either a nested table
// { ip = <string>, port = <number> } or a single "host:port" string.

#include "tls_config.hpp"

#include <cstdint>
#include <string>

struct endpoint {
  std::string host;
  std::uint16_t port{0};
};

struct peer_config {
  endpoint local;
  endpoint remote;
  tls_config tls; // enabled=false unless a tls table is present
};

// Parse `path` into `out`. Returns true on success; on failure returns false
// and writes a human-readable reason into `err`.
bool load_peer_config(const std::string &path, peer_config &out,
                      std::string &err);

#endif // __endpoint_config_hpp__
