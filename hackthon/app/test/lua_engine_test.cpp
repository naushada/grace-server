// Unit tests for the lua_file class declared in app/inc/lua_engine.hpp.
//
// These tests build a temporary directory, drop .lua files into it, and
// exercise the create/delete/modify API of lua_file. They cover the three
// variant shapes currently produced by parse_lua_to_table (scalar,
// array-of-scalars, nested table) and verify the corresponding std::variant
// alternatives are populated as expected.

#include "lua_engine.hpp"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

namespace {

class LuaEngineTest : public ::testing::Test {
protected:
  std::filesystem::path tmp_dir;

  void SetUp() override {
    const auto name =
        "lua_engine_test_" +
        std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
        "_" + ::testing::UnitTest::GetInstance()->current_test_info()->name();
    tmp_dir = std::filesystem::temp_directory_path() / name;
    std::filesystem::create_directories(tmp_dir);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
  }

  // Writes `body` to <tmp_dir>/<name> and returns the absolute path.
  std::string write_lua(const std::string &name, const std::string &body) {
    auto path = tmp_dir / name;
    std::ofstream os(path);
    os << body;
    os.close();
    return path.string();
  }

  // Shortcut: assume the file exposes a single top-level table whose first
  // key is `cmd`, and return a reference to that nested parameter table.
  static const lua_file::table_type &
  cmd_table(const lua_file &engine, const std::string &path,
            const std::string &cmd) {
    const auto &file_root = engine.commands().at(path);
    const auto &entry = file_root.members.at(cmd);
    const auto &ptr =
        std::get<std::shared_ptr<lua_file::table_type>>(entry);
    return *ptr;
  }
};

TEST_F(LuaEngineTest, LoadsEmptyTopTable) {
  lua_file engine;
  auto path = write_lua("empty.lua", "return {}");
  engine.process_create_luafile(path);

  ASSERT_EQ(engine.commands().count(path), 1u);
  EXPECT_TRUE(engine.commands().at(path).members.empty());
}

TEST_F(LuaEngineTest, LoadsScalarTypes) {
  lua_file engine;
  auto path = write_lua("scalars.lua", R"LUA(
    return {
      MyCmd = {
        an_int   = 42,
        a_double = 3.14,
        a_string = "hello",
        a_bool   = true,
      }
    }
  )LUA");
  engine.process_create_luafile(path);

  const auto &members = cmd_table(engine, path, "MyCmd").members;
  ASSERT_EQ(members.count("an_int"), 1u);
  ASSERT_EQ(members.count("a_double"), 1u);
  ASSERT_EQ(members.count("a_string"), 1u);
  ASSERT_EQ(members.count("a_bool"), 1u);

  const auto *vi = std::get_if<lua_file::value_type>(&members.at("an_int"));
  ASSERT_NE(vi, nullptr);
  EXPECT_EQ(std::get<std::int32_t>(*vi), 42);

  const auto *vd = std::get_if<lua_file::value_type>(&members.at("a_double"));
  ASSERT_NE(vd, nullptr);
  EXPECT_DOUBLE_EQ(std::get<double>(*vd), 3.14);

  const auto *vs = std::get_if<lua_file::value_type>(&members.at("a_string"));
  ASSERT_NE(vs, nullptr);
  EXPECT_EQ(std::get<std::string>(*vs), "hello");

  const auto *vb = std::get_if<lua_file::value_type>(&members.at("a_bool"));
  ASSERT_NE(vb, nullptr);
  EXPECT_TRUE(std::get<bool>(*vb));
}

TEST_F(LuaEngineTest, LoadsArrayOfStrings) {
  lua_file engine;
  auto path = write_lua("array.lua", R"LUA(
    return { MyCmd = { ids = {"a", "b", "c"} } }
  )LUA");
  engine.process_create_luafile(path);

  const auto &members = cmd_table(engine, path, "MyCmd").members;
  const auto *vec =
      std::get_if<std::vector<lua_file::value_type>>(&members.at("ids"));
  ASSERT_NE(vec, nullptr);
  ASSERT_EQ(vec->size(), 3u);
  EXPECT_EQ(std::get<std::string>((*vec)[0]), "a");
  EXPECT_EQ(std::get<std::string>((*vec)[1]), "b");
  EXPECT_EQ(std::get<std::string>((*vec)[2]), "c");
}

TEST_F(LuaEngineTest, LoadsNestedTable) {
  lua_file engine;
  auto path = write_lua("nested.lua", R"LUA(
    return {
      MyCmd = {
        outer = {
          inner = { leaf = 99 }
        }
      }
    }
  )LUA");
  engine.process_create_luafile(path);

  const auto &members = cmd_table(engine, path, "MyCmd").members;
  const auto &outer_ptr =
      std::get<std::shared_ptr<lua_file::table_type>>(members.at("outer"));
  ASSERT_NE(outer_ptr, nullptr);

  const auto &inner_ptr = std::get<std::shared_ptr<lua_file::table_type>>(
      outer_ptr->members.at("inner"));
  ASSERT_NE(inner_ptr, nullptr);

  const auto *leaf =
      std::get_if<lua_file::value_type>(&inner_ptr->members.at("leaf"));
  ASSERT_NE(leaf, nullptr);
  EXPECT_EQ(std::get<std::int32_t>(*leaf), 99);
}

TEST_F(LuaEngineTest, DeleteRemovesEntry) {
  lua_file engine;
  auto path = write_lua("x.lua", "return { MyCmd = {} }");
  engine.process_create_luafile(path);
  ASSERT_EQ(engine.commands().count(path), 1u);

  engine.process_delete_luafile(path);
  EXPECT_EQ(engine.commands().count(path), 0u);
}

TEST_F(LuaEngineTest, ModifyReloadsContent) {
  lua_file engine;
  auto path = write_lua("m.lua", "return { MyCmd = { v = 1 } }");
  engine.process_create_luafile(path);

  // Overwrite the file with a new value and ask the engine to reload.
  write_lua("m.lua", "return { MyCmd = { v = 2 } }");
  engine.process_modify_luafile(path);

  const auto &members = cmd_table(engine, path, "MyCmd").members;
  const auto *v = std::get_if<lua_file::value_type>(&members.at("v"));
  ASSERT_NE(v, nullptr);
  EXPECT_EQ(std::get<std::int32_t>(*v), 2);
}

TEST_F(LuaEngineTest, MissingFileIsSafe) {
  lua_file engine;
  const std::string bogus =
      (tmp_dir / "does_not_exist_xyz_abc.lua").string();
  EXPECT_NO_THROW(engine.process_create_luafile(bogus));
  EXPECT_EQ(engine.commands().count(bogus), 0u);
}

TEST_F(LuaEngineTest, MultipleFilesCoexist) {
  lua_file engine;
  auto a = write_lua("a.lua", "return { A = { x = 1 } }");
  auto b = write_lua("b.lua", "return { B = { y = 2 } }");
  engine.process_create_luafile(a);
  engine.process_create_luafile(b);

  EXPECT_EQ(engine.commands().size(), 2u);
  EXPECT_EQ(std::get<std::int32_t>(
                std::get<lua_file::value_type>(
                    cmd_table(engine, a, "A").members.at("x"))),
            1);
  EXPECT_EQ(std::get<std::int32_t>(
                std::get<lua_file::value_type>(
                    cmd_table(engine, b, "B").members.at("y"))),
            2);
}

} // namespace
