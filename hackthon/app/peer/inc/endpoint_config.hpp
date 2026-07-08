// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

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

// TUI colour names for each line category (see gnmi_tui for how they are
// mapped to terminal attributes). Accepts base colours (black/red/green/
// yellow/blue/magenta/cyan/white), the aliases amber(=yellow)/grey/gray, the
// attributes dim/bold, bright-<colour>, and default/none. Defaults below match
// the built-in cool palette.
struct palette_config {
  std::string remote = "cyan";  // [remote] pushes (the "diff")
  std::string ok = "green";     // [set]/[get] OK
  std::string error = "amber";  // errors / usage
  std::string echo = "dim";     // echoed commands
};

struct peer_config {
  endpoint local;
  endpoint remote;
  tls_config tls;       // enabled=false unless a tls table is present
  palette_config colors; // defaults unless a `colors` table is present
};

// Parse `path` into `out`. Returns true on success; on failure returns false
// and writes a human-readable reason into `err`.
bool load_peer_config(const std::string &path, peer_config &out,
                      std::string &err);

#endif // __endpoint_config_hpp__
