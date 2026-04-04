#ifndef __lua_engine_cpp__
#define __lua_engine_cpp__

#include "lua_engine.hpp"

bool lua_file::is_lua_array(lua_State *L, int index) {
  lua_pushnil(L);
  bool has_items = lua_next(L, index - 1) != 0;
  if (has_items) {
    bool is_array =
        lua_isnumber(L, -2); // If first key is a number, treat as array
    lua_pop(L, 2);           // Pop key and value
    return is_array;
  }
  return false;
}

void lua_file::parse_lua_to_table(lua_State *L, std::int32_t index,
                                  table_type &out_table) {
  lua_pushnil(L); // Start iteration
  while (lua_next(L, index - 1) != 0) {
    // Key is at -2, Value is at -1
    if (lua_isstring(L, -2)) {
      std::string key = lua_tostring(L, -2);

      if (lua_istable(L, -1)) {
        if (is_lua_array(L, lua_gettop(L))) {
          // Handle Arrays (e.g., rn_device_ids = {1, 2})
          std::vector<value_type> vec;
          lua_pushnil(L);
          while (lua_next(L, -2) != 0) {
            vec.push_back(
                value_type(lua_tostring(L, -1))); // Simplified to string
            lua_pop(L, 1);
          }
          out_table.members[key] = vec;
        } else {
          // Handle Nested Tables (e.g., test_param = { ... })
          auto sub_table = std::make_shared<table_type>();
          parse_lua_to_table(L, lua_gettop(L), *sub_table);
          out_table.members[key] = sub_table;
        }
      } else {
        // Handle Basic Values (Singular fields)
        if (lua_isboolean(L, -1))
          out_table.members[key] = value_type((bool)lua_toboolean(L, -1));
        else if (lua_isinteger(L, -1))
          out_table.members[key] = value_type((int32_t)lua_tointeger(L, -1));
        else if (lua_isnumber(L, -1))
          out_table.members[key] = value_type((double)lua_tonumber(L, -1));
        else if (lua_isstring(L, -1))
          out_table.members[key] = value_type(std::string(lua_tostring(L, -1)));
      }
    }
    lua_pop(L, 1); // Pop value, keep key for next iteration
  }
}

void lua_file::process_create_luafile(const std::string &file_name) {
  if (luaL_dofile(m_luaL.get(), file_name.c_str()) == LUA_OK) {
    table_type file_root;
    if (lua_istable(m_luaL.get(), -1)) {
      // Use the recursive parser we built
      parse_lua_to_table(m_luaL.get(), lua_gettop(m_luaL.get()), file_root);

      // Store it: e.g., m_commands["test_config.lua"] = file_root;
      m_commands[file_name] = std::move(file_root);
    }
    lua_pop(m_luaL.get(), 1); // Clean stack
  }
}

void lua_file::process_delete_luafile(const std::string &file_name) {
  auto it = m_commands.find(file_name);
  if (it != m_commands.end()) {
    m_commands.erase(it);
  }
}

void lua_file::process_modify_luafile(const std::string &file_name) {
  process_delete_luafile(file_name);
  process_create_luafile(file_name);
}

#endif
