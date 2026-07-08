// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#include "tunnel_config.hpp"
#include "lua_engine.hpp"

#include <cstdlib>
#include <memory>
#include <variant>

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
static bool val_str(const value_type &v, std::string &out) {
  if (std::holds_alternative<std::string>(v)) {
    out = std::get<std::string>(v);
    return true;
  }
  return false;
}
static bool val_int(const value_type &v, long &out) {
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
static bool val_bool(const value_type &v, bool &out) {
  if (std::holds_alternative<bool>(v)) {
    out = std::get<bool>(v);
    return true;
  }
  return false;
}
static void opt_str(const table_type &t, const std::string &key,
                    std::string &out) {
  if (const entry_type *e = find_member(t, key))
    if (const value_type *v = as_value(*e))
      val_str(*v, out);
}

bool load_tunnel_config(const std::string &path, tunnel_config &out,
                        std::string &err) {
  lua_file cfg;
  cfg.process_create_luafile(path);
  auto it = cfg.commands().find(path);
  if (it == cfg.commands().end()) {
    err = "failed to load Lua config: " + path;
    return false;
  }
  const table_type &root = it->second;

  // Optional control port (default 58989).
  if (const entry_type *e = find_member(root, "port"))
    if (const value_type *v = as_value(*e)) {
      long p = 0;
      if (val_int(*v, p) && p > 0 && p <= 65535)
        out.port = static_cast<std::uint16_t>(p);
    }

  // Optional tls table.
  if (const entry_type *tE = find_member(root, "tls"))
    if (const table_type *tT = as_table(*tE)) {
      if (const entry_type *en = find_member(*tT, "enabled"))
        if (const value_type *v = as_value(*en))
          val_bool(*v, out.tls.enabled);
      opt_str(*tT, "cert", out.tls.cert_file);
      opt_str(*tT, "key", out.tls.key_file);
      opt_str(*tT, "ca", out.tls.ca_file);
    }

  // Listeners: map of "port" -> target.
  if (const entry_type *lE = find_member(root, "listeners"))
    if (const table_type *lT = as_table(*lE)) {
      for (const auto &kv : lT->members) {
        const long p = std::strtol(kv.first.c_str(), nullptr, 10);
        if (p <= 0 || p > 65535) {
          err = "listener port key invalid: '" + kv.first + "'";
          return false;
        }
        std::string target;
        if (const value_type *v = as_value(kv.second))
          val_str(*v, target);
        if (target.empty()) {
          err = "listener '" + kv.first + "' has no target string";
          return false;
        }
        out.listeners.push_back({static_cast<std::uint16_t>(p), target});
      }
    }

  return true;
}
