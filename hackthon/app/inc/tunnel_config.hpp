#ifndef __tunnel_config_hpp__
#define __tunnel_config_hpp__

// grpc-tunnel-server config, loaded from a Lua file (--config <file.lua>).
//
// Lets one server front several devices — a data-plane listener per published
// target. The listener list is a map of local-port -> target name (the Lua
// engine can't represent an array of tables, so port is a string table key):
//
//   return {
//     port = 58989,                                    -- control/tunnel port
//     tls  = { enabled = false, cert = "", key = "", ca = "" },
//     listeners = {                                     -- "port" -> target
//       ["9339"] = "<published-target-A>",
//       ["9340"] = "<published-target-B>",
//     },
//   }

#include "tls_config.hpp"

#include <cstdint>
#include <string>
#include <vector>

struct tunnel_listener_cfg {
  std::uint16_t port{0};
  std::string target;
};

struct tunnel_config {
  std::uint16_t port{58989}; // control/tunnel listen port
  tls_config tls;            // enabled=false unless a tls table is present
  std::vector<tunnel_listener_cfg> listeners;
};

// Parse `path` into `out`. Returns true on success; on failure returns false and
// writes a human-readable reason into `err`.
bool load_tunnel_config(const std::string &path, tunnel_config &out,
                        std::string &err);

#endif // __tunnel_config_hpp__
