#ifndef __lua_engine_hpp__
#define __lua_engine_hpp__

#include <algorithm>
#include <iostream>
#include <lua.hpp>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

class lua_file {
  struct custom_deleter {
    void operator()(lua_State *l) { lua_close(l); }
  };

public:
  lua_file() : m_luaL(luaL_newstate()) { luaL_openlibs(m_luaL.get()); }

  ~lua_file() {
    m_luaL.reset(nullptr);
    m_commands.clear();
  }

  // 1. Basic m_luaL.geteaf Values (Singular fields in Proto)
  struct value_type : public std::variant<std::nullptr_t, std::string, bool,
                                          std::uint32_t, std::int32_t, double> {
    using variant::variant;
    value_type() : variant(nullptr) {}
  };

  struct table_type; // Forward declaration for recursion

  // 2. The Container Type (Maps to Messages/Repeated in Proto)
  using entry_type = std::variant<
      value_type,                  // Singular: int32, string, etc.
      std::vector<value_type>,     // Repeated: repeated int32, repeated string
      std::shared_ptr<table_type>, // Nested: message FieldName { ... }
      std::vector<std::shared_ptr<table_type>> // Repeated Message: repeated
                                               // MyMessage
      >;

  struct table_type {
    std::unordered_map<std::string, entry_type> members;
  };

  value_type extract_value(lua_State *L, std::int32_t idx);
  bool is_lua_array(lua_State *L, std::int32_t index);
  void parse_lua_to_table(lua_State *L, std::int32_t index,
                          table_type &out_table);
  void process_create_luafile(const std::string &file_name);
  void process_delete_luafile(const std::string &file_name);
  void process_modify_luafile(const std::string &file_name);
  void dump_commands();
  void dump_table(const table_type &table, int indent = 0);
  const std::unordered_map<std::string, lua_file::table_type> &commands() const;

private:
  std::unique_ptr<lua_State, custom_deleter> m_luaL;
  std::unordered_map<std::string, table_type> m_commands;
};

#endif
