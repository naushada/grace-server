// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

// Tests for the CLI banner string and the `help` built-in output.
// These deliberately go through help.hpp (not readline.cpp), so the test
// binary does not need to link against readline / protobuf / mosquitto.

#include "help.hpp"
#include "lua_engine.hpp"

#include <gtest/gtest.h>
#include <map>
#include <sstream>
#include <string>

namespace {
bool contains(const std::string &haystack, const std::string &needle) {
  return haystack.find(needle) != std::string::npos;
}
} // namespace

TEST(CliBanner, ExactMarvelString) {
  // Tripwire — if the banner is reworded, update this assertion deliberately.
  EXPECT_STREQ(kCliBanner,
               "Marvel gNMI CLI - ready (type 'help' for commands)");
}

TEST(CliHelp, BuiltInsAreListed) {
  std::ostringstream os;
  print_help(os, {});
  const std::string out = os.str();
  EXPECT_TRUE(contains(out, "clients"));
  EXPECT_TRUE(contains(out, "help"));
}

TEST(CliHelp, AllGnmiCommandsAreListed) {
  std::ostringstream os;
  print_help(os, {});
  const std::string out = os.str();
  EXPECT_TRUE(contains(out, "gnmi_get"));
  EXPECT_TRUE(contains(out, "gnmi_update"));
  EXPECT_TRUE(contains(out, "gnmi_replace"));
  EXPECT_TRUE(contains(out, "gnmi_delete"));
}

TEST(CliHelp, OmitsLuaSectionWhenNoLuaCommandsLoaded) {
  std::ostringstream os;
  print_help(os, {});
  EXPECT_FALSE(contains(os.str(), "Lua commands loaded"));
}

TEST(CliHelp, ListsLuaCommandsWhenPresent) {
  std::map<std::string, lua_file::table_type> cmds;
  lua_file::table_type t;
  t.members["StartDownlinkConnectionsTestRequest"] = {};
  t.members["AnotherLuaCommand"]                   = {};
  cmds["StartDownlinkConnectionsTestRequest.lua"]  = t;

  std::ostringstream os;
  print_help(os, cmds);
  const std::string out = os.str();
  EXPECT_TRUE(contains(out, "Lua commands loaded"));
  EXPECT_TRUE(contains(out, "StartDownlinkConnectionsTestRequest"));
  EXPECT_TRUE(contains(out, "AnotherLuaCommand"));
}
