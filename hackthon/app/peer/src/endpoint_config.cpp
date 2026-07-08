// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#include "endpoint_config.hpp"
#include "lua_engine.hpp"

#include <cstdlib>
#include <memory>
#include <variant>

// ---------------------------------------------------------------------------
// Low-level accessors over the lua_file data model.
//
//   table_type.members : map<string, entry_type>
//   entry_type         : variant<value_type, vector<value_type>,
//                                 shared_ptr<table_type>, vector<shared_ptr<...>>>
//   value_type         : variant<nullptr_t, string, bool, uint32, int32, double>
//
// entry_type is a plain std::variant, so std::get_if works. value_type derives
// from std::variant, so we read it via holds_alternative/std::get on a
// reference (the same pattern readline.cpp uses) — std::get_if on a value_type*
// would fail template deduction.
// ---------------------------------------------------------------------------

using table_type = lua_file::table_type;
using entry_type = lua_file::entry_type;
using value_type = lua_file::value_type;

static const entry_type *find_member(const table_type &t,
                                     const std::string &key) {
  auto it = t.members.find(key);
  return it == t.members.end() ? nullptr : &it->second;
}

static const table_type *as_table(const entry_type &e) {
  if (auto *p = std::get_if<std::shared_ptr<table_type>>(&e))
    return p->get();
  return nullptr;
}

static const value_type *as_value(const entry_type &e) {
  return std::get_if<value_type>(&e);
}

static bool value_string(const value_type &v, std::string &out) {
  if (std::holds_alternative<std::string>(v)) {
    out = std::get<std::string>(v);
    return true;
  }
  return false;
}

static bool value_int(const value_type &v, long &out) {
  if (std::holds_alternative<std::int32_t>(v)) {
    out = std::get<std::int32_t>(v);
    return true;
  }
  if (std::holds_alternative<std::uint32_t>(v)) {
    out = static_cast<long>(std::get<std::uint32_t>(v));
    return true;
  }
  if (std::holds_alternative<double>(v)) {
    out = static_cast<long>(std::get<double>(v));
    return true;
  }
  return false;
}

static bool value_bool(const value_type &v, bool &out) {
  if (std::holds_alternative<bool>(v)) {
    out = std::get<bool>(v);
    return true;
  }
  return false;
}

// Read an optional string member `key` from table `t` into `out`.
static void read_opt_string(const table_type &t, const std::string &key,
                            std::string &out) {
  if (const entry_type *e = find_member(t, key))
    if (const value_type *v = as_value(*e))
      value_string(*v, out);
}

// ---------------------------------------------------------------------------
// endpoint resolution — accepts { ip=, port= } table OR "host:port" string.
// ---------------------------------------------------------------------------

static bool port_from_long(long p, std::uint16_t &out, std::string &err,
                           const char *who) {
  if (p <= 0 || p > 65535) {
    err = std::string(who) + ".endpoint port out of range: " +
          std::to_string(p);
    return false;
  }
  out = static_cast<std::uint16_t>(p);
  return true;
}

static bool resolve_endpoint(const table_type &side, endpoint &out,
                             std::string &err, const char *who) {
  const entry_type *ep = find_member(side, "endpoint");
  if (!ep) {
    err = std::string(who) + ".endpoint is missing";
    return false;
  }

  // "host:port" string form.
  if (const value_type *v = as_value(*ep)) {
    std::string s;
    if (!value_string(*v, s)) {
      err = std::string(who) + ".endpoint must be a string or table";
      return false;
    }
    const auto colon = s.rfind(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= s.size()) {
      err = std::string(who) + ".endpoint must be \"host:port\": " + s;
      return false;
    }
    out.host = s.substr(0, colon);
    const long p = std::strtol(s.c_str() + colon + 1, nullptr, 10);
    return port_from_long(p, out.port, err, who);
  }

  // { ip = <string>, port = <number> } table form.
  if (const table_type *t = as_table(*ep)) {
    const entry_type *ipE = find_member(*t, "ip");
    const entry_type *portE = find_member(*t, "port");
    std::string ip;
    long port = 0;
    if (!ipE || !as_value(*ipE) || !value_string(*as_value(*ipE), ip)) {
      err = std::string(who) + ".endpoint.ip missing or not a string";
      return false;
    }
    if (!portE || !as_value(*portE) || !value_int(*as_value(*portE), port)) {
      err = std::string(who) + ".endpoint.port missing or not a number";
      return false;
    }
    out.host = ip;
    return port_from_long(port, out.port, err, who);
  }

  err = std::string(who) + ".endpoint has an unexpected type";
  return false;
}

// ---------------------------------------------------------------------------
// load_peer_config
// ---------------------------------------------------------------------------

bool load_peer_config(const std::string &path, peer_config &out,
                      std::string &err) {
  lua_file cfg;
  cfg.process_create_luafile(path);

  // process_create_luafile only inserts on successful load; absence => failure.
  auto it = cfg.commands().find(path);
  if (it == cfg.commands().end()) {
    err = "failed to load Lua config: " + path;
    return false;
  }
  const table_type &root = it->second;

  const entry_type *localE = find_member(root, "local");
  const table_type *localT = localE ? as_table(*localE) : nullptr;
  if (!localT) {
    err = "[\"local\"] table missing (use [\"local\"] = { endpoint = ... })";
    return false;
  }
  if (!resolve_endpoint(*localT, out.local, err, "local"))
    return false;

  const entry_type *remoteE = find_member(root, "remote");
  const table_type *remoteT = remoteE ? as_table(*remoteE) : nullptr;
  if (!remoteT) {
    err = "[\"remote\"] table missing (use [\"remote\"] = { endpoint = ... })";
    return false;
  }
  if (!resolve_endpoint(*remoteT, out.remote, err, "remote"))
    return false;

  // Optional colors table (TUI palette). Missing keys keep their defaults.
  if (const entry_type *colE = find_member(root, "colors")) {
    if (const table_type *colT = as_table(*colE)) {
      read_opt_string(*colT, "remote", out.colors.remote);
      read_opt_string(*colT, "ok", out.colors.ok);
      read_opt_string(*colT, "error", out.colors.error);
      read_opt_string(*colT, "echo", out.colors.echo);
    }
  }

  // Optional tls table.
  if (const entry_type *tlsE = find_member(root, "tls")) {
    if (const table_type *tlsT = as_table(*tlsE)) {
      if (const entry_type *en = find_member(*tlsT, "enabled"))
        if (const value_type *v = as_value(*en))
          value_bool(*v, out.tls.enabled);
      read_opt_string(*tlsT, "cert", out.tls.cert_file);
      read_opt_string(*tlsT, "key", out.tls.key_file);
      read_opt_string(*tlsT, "ca", out.tls.ca_file);
    }
  }

  return true;
}
