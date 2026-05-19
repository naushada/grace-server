#ifndef __cli_help_hpp__
#define __cli_help_hpp__

#include "lua_engine.hpp"

#include <map>
#include <ostream>
#include <string>

// Banner line printed once at REPL startup.  Kept out of readline.cpp so
// tests can assert on the literal without dragging in readline/protobuf.
extern const char *const kCliBanner;

// Writes the human-facing help text to `os`.  Lists built-in commands, the
// gNMI command shapes, and any Lua commands loaded via fs_app/lua_engine
// (passed in directly so the function has no global dependency).
void print_help(std::ostream &os,
                const std::map<std::string, lua_file::table_type> &lua_commands);

#endif // __cli_help_hpp__
