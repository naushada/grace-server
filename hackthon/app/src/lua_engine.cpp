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

lua_file::value_type lua_file::extract_value(lua_State *L, int idx) {
  if (lua_isboolean(L, idx))
    return value_type((bool)lua_toboolean(L, idx));
  if (lua_isinteger(L, idx))
    return value_type((std::int32_t)lua_tointeger(L, idx));
  if (lua_isnumber(L, idx))
    return value_type((double)lua_tonumber(L, idx));
  if (lua_isstring(L, idx))
    return value_type(std::string(lua_tostring(L, idx)));
  // Fallback for nil or unsupported types
  return value_type(nullptr);
}

void lua_file::parse_lua_to_table(lua_State *L, std::int32_t table_index,
                                  table_type &out_table) {
  // 1. Ensure table_index is absolute (prevents issues if the stack grows)
  if (table_index < 0)
    table_index = lua_gettop(L) + table_index + 1;

  lua_pushnil(L);
  while (lua_next(L, table_index) != 0) {
    // Key is at -2, Value is at -1
    if (lua_isstring(L, -2)) {
      std::string key = lua_tostring(L, -2);

      if (lua_istable(L, -1)) {
        // Determine if it's an Array (numeric index 1 exists) or a Table
        lua_rawgeti(L, -1, 1);
        bool is_array = !lua_isnil(L, -1);
        lua_pop(L, 1);

        if (is_array) {
          // --- CASE 1: ARRAY --- (device_id = {"1", "2"})
          std::vector<value_type> vec;
          int len = lua_rawlen(L, -1);
          for (int i = 1; i <= len; ++i) {
            lua_rawgeti(L, -1, i);
            // Convert stack -1 to value_type (string, int, etc.)
            vec.push_back(extract_value(L, -1));
            lua_pop(L, 1);
          }
          out_table.members[key] = vec;
        } else {
          // --- CASE 2: NESTED TABLE --- (test_param = { ... })
          auto sub_table = std::make_shared<table_type>();
          parse_lua_to_table(L, lua_gettop(L), *sub_table);
          out_table.members[key] = sub_table;
        }
      } else {
        // --- CASE 3: ELEMENTARY FIELD --- (rate_mbps = 50)
        out_table.members[key] = extract_value(L, -1);
      }
    }
    lua_pop(L, 1); // Pop value, keep key for next lua_next
  }
}

void lua_file::process_create_luafile(const std::string &file_name) {
  if (luaL_dofile(m_luaL.get(), file_name.c_str()) == LUA_OK) {
    table_type file_root;
    int top = lua_gettop(m_luaL.get());
    // Only parse when the script returned a table. Scripts that only assign
    // globals return nothing (top==0); passing 0 to lua_next is invalid and
    // crashes. Parsing _G is also unsafe because _G._G is self-referential.
    if (top > 0 && lua_istable(m_luaL.get(), top)) {
      parse_lua_to_table(m_luaL.get(), top, file_root);
    }
    lua_settop(m_luaL.get(), 0);
    m_commands[file_name] = std::move(file_root);
  } else {
    std::cout << "Fn:" << __func__ << ":" << __LINE__
              << " Unable to load the file:" << file_name << std::endl;
    lua_pop(m_luaL.get(), 1); // discard the error string left by luaL_dofile
  }
}

void lua_file::process_delete_luafile(const std::string &file_name) {
  auto it = m_commands.find(file_name);
  if (it != m_commands.end()) {
    m_commands.erase(it);
  }
}

void lua_file::dump_table(const table_type &table, int indent) {
  std::string padding(indent, ' ');

  for (const auto &[key, entry] : table.members) {
    std::cout << padding << key << " = ";

    std::visit(
        [&](auto &&arg) {
          using T = std::decay_t<decltype(arg)>;

          if constexpr (std::is_same_v<T, value_type>) {
            // Leaf value (string, int, bool, etc.)
            std::visit(
                [](auto &&val) {
                  using V = std::decay_t<decltype(val)>;
                  if constexpr (std::is_same_v<V, std::nullptr_t>)
                    std::cout << "nil";
                  else if constexpr (std::is_same_v<V, bool>)
                    std::cout << (val ? "true" : "false");
                  else
                    std::cout << val;
                },
                arg);
            std::cout << "\n";
          } else if constexpr (std::is_same_v<T, std::vector<value_type>>) {
            // Simple Array
            std::cout << "{ ";
            for (const auto &v : arg) {
              std::visit([](auto &&val) { std::cout << val << " "; }, v);
            }
            std::cout << "}\n";
          } else if constexpr (std::is_same_v<T, std::shared_ptr<table_type>>) {
            // Nested Table
            std::cout << "{\n";
            if (arg)
              dump_table(*arg, indent + 4);
            std::cout << padding << "}\n";
          } else if constexpr (std::is_same_v<
                                   T,
                                   std::vector<std::shared_ptr<table_type>>>) {
            // Array of Tables
            std::cout << "[List of Tables]\n";
            for (const auto &sub : arg) {
              if (sub)
                dump_table(*sub, indent + 4);
            }
          }
        },
        entry);
  }
}

void lua_file::dump_commands() {
  for (const auto &[key, value] : m_commands) {
    std::cout << "key:" << key << std::endl;
    dump_table(value);
  }
}

void lua_file::process_modify_luafile(const std::string &file_name) {
  process_delete_luafile(file_name);
  process_create_luafile(file_name);
}

const std::map<std::string, lua_file::table_type> &lua_file::commands() const {
  return m_commands;
}

void lua_file::write_table(
    const std::string &path,
    const std::string &top_key,
    const std::vector<std::pair<std::string, std::string>> &fields) {
  std::ofstream f(path, std::ios::trunc);
  if (!f) {
    std::cerr << "[lua_file] cannot write " << path << "\n";
    return;
  }
  f << "-- auto-generated by lua_file::write_table\n"
    << "return {\n"
    << "  " << top_key << " = {\n";
  for (const auto &[k, v] : fields)
    f << "    " << k << " = " << v << ",\n";
  f << "  },\n}\n";
}

#endif
