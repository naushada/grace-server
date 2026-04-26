#ifndef __lua_engine_hpp__
#define __lua_engine_hpp__

#include <algorithm>
#include <fstream>
#include <iostream>
#include <lua.hpp>
#include <map>
#include <memory>
#include <string>
#include <utility>
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
    // Type aliases for easier access
    using iterator = std::map<std::string, entry_type>::iterator;
    using const_iterator = std::map<std::string, entry_type>::const_iterator;

    std::map<std::string, entry_type> members;
    // Non-const iterators for modification
    iterator begin() { return members.begin(); }
    iterator end() { return members.end(); }

    // Const iterators for read-only access
    const_iterator begin() const { return members.begin(); }
    const_iterator end() const { return members.end(); }
    const_iterator cbegin() const { return members.cbegin(); }
    const_iterator cend() const { return members.cend(); }

    // find_if member function
    template <typename Predicate> iterator find_if(Predicate pred) {
      return std::find_if(members.begin(), members.end(), pred);
    }

    // Const version for read-only access
    template <typename Predicate> const_iterator find_if(Predicate pred) const {
      return std::find_if(members.begin(), members.end(), pred);
    }
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
  const std::map<std::string, lua_file::table_type> &commands() const;

  // Write a single-table Lua file.  Each field pair is (key, lua_value) where
  // lua_value is already valid Lua syntax (quoted strings include the quotes;
  // numbers are bare).  The file is valid `return { top_key = { ... } }` Lua.
  static void write_table(
      const std::string &path,
      const std::string &top_key,
      const std::vector<std::pair<std::string, std::string>> &fields);

private:
  std::unique_ptr<lua_State, custom_deleter> m_luaL;
  // key is the name of lua file and value is lua table
  std::map<std::string, table_type> m_commands;
};

#endif
