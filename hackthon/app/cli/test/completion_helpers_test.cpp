// Unit tests for the Tab-completion helper functions declared in
// app/cli/inc/completion_helpers.hpp.
//
// The tests build lua_file::table_type fixtures two ways:
//   1. Hand-constructed in memory — keeps the cases minimal and fast.
//   2. Loaded from a real .lua file via lua_file — exercises the integration
//      path the CLI actually takes.

#include "completion_helpers.hpp"
#include "lua_engine.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

namespace {

// --------------------------------------------------------------------------
// Helpers
// --------------------------------------------------------------------------

// Returns true if `haystack` contains an element equal to `needle`.
bool contains(const std::vector<std::string> &haystack,
              const std::string &needle) {
  return std::find(haystack.begin(), haystack.end(), needle) != haystack.end();
}

// Build a small table_type by hand — no Lua runtime involved.
//
//   scalar_int     = 7
//   scalar_bool    = true
//   list_of_ints   = {1, 2, 3}
//   nested = {
//     inner_str  = "abc",
//     inner_tbl  = { deepest = 99 }
//   }
std::shared_ptr<lua_file::table_type> build_fixture_table() {
  auto root = std::make_shared<lua_file::table_type>();

  root->members["scalar_int"] =
      lua_file::value_type(static_cast<std::int32_t>(7));
  root->members["scalar_bool"] = lua_file::value_type(true);

  std::vector<lua_file::value_type> ints;
  ints.emplace_back(static_cast<std::int32_t>(1));
  ints.emplace_back(static_cast<std::int32_t>(2));
  ints.emplace_back(static_cast<std::int32_t>(3));
  root->members["list_of_ints"] = ints;

  auto nested = std::make_shared<lua_file::table_type>();
  nested->members["inner_str"] = lua_file::value_type(std::string("abc"));

  auto inner_tbl = std::make_shared<lua_file::table_type>();
  inner_tbl->members["deepest"] =
      lua_file::value_type(static_cast<std::int32_t>(99));
  nested->members["inner_tbl"] = inner_tbl;

  root->members["nested"] = nested;
  return root;
}

// --------------------------------------------------------------------------
// Pure unit tests against a hand-built table
// --------------------------------------------------------------------------

TEST(GetSubKeys, EmptyFilterListsEverythingReachable) {
  auto root = build_fixture_table();

  std::vector<std::string> matches;
  get_sub_keys(*root, "", "", matches);

  EXPECT_TRUE(contains(matches, "scalar_int="));
  EXPECT_TRUE(contains(matches, "scalar_bool="));
  EXPECT_TRUE(contains(matches, "list_of_ints="));
  EXPECT_TRUE(contains(matches, "nested."));
  EXPECT_TRUE(contains(matches, "nested.inner_str="));
  EXPECT_TRUE(contains(matches, "nested.inner_tbl."));
  EXPECT_TRUE(contains(matches, "nested.inner_tbl.deepest="));
}

TEST(GetSubKeys, ArrayOfScalarsIsLeaf) {
  auto root = build_fixture_table();

  std::vector<std::string> matches;
  get_sub_keys(*root, "", "list_of_ints", matches);

  // It should appear once as a leaf with "=" — not as "list_of_ints.".
  EXPECT_TRUE(contains(matches, "list_of_ints="));
  EXPECT_FALSE(contains(matches, "list_of_ints."));
}

TEST(GetSubKeys, FilterPrunesUnrelatedBranches) {
  auto root = build_fixture_table();

  std::vector<std::string> matches;
  get_sub_keys(*root, "", "nested.", matches);

  // Only the nested branch's descendants should survive.
  for (const auto &m : matches) {
    EXPECT_EQ(m.compare(0, std::string("nested.").size(), "nested."), 0)
        << "Unexpected match leaked through filter: " << m;
  }
  EXPECT_TRUE(contains(matches, "nested.inner_str="));
  EXPECT_TRUE(contains(matches, "nested.inner_tbl."));
  EXPECT_TRUE(contains(matches, "nested.inner_tbl.deepest="));
  EXPECT_FALSE(contains(matches, "scalar_int="));
  EXPECT_FALSE(contains(matches, "scalar_bool="));
  EXPECT_FALSE(contains(matches, "list_of_ints="));
}

TEST(GetSubKeys, PartialWordDoesNotMatchUnrelatedSiblings) {
  auto root = build_fixture_table();

  std::vector<std::string> matches;
  get_sub_keys(*root, "", "scal", matches);

  EXPECT_TRUE(contains(matches, "scalar_int="));
  EXPECT_TRUE(contains(matches, "scalar_bool="));
  EXPECT_FALSE(contains(matches, "list_of_ints="));
  EXPECT_FALSE(contains(matches, "nested."));
}

TEST(GetSubKeys, DeepPrefixDrillsThroughParent) {
  auto root = build_fixture_table();

  std::vector<std::string> matches;
  get_sub_keys(*root, "", "nested.inner_tbl.", matches);

  EXPECT_TRUE(contains(matches, "nested.inner_tbl.deepest="));
  EXPECT_FALSE(contains(matches, "nested.inner_str="));
}

TEST(GetSubKeys, EmptyTableProducesEmptyMatches) {
  lua_file::table_type empty;
  std::vector<std::string> matches;
  get_sub_keys(empty, "", "anything", matches);
  EXPECT_TRUE(matches.empty());
}

// --------------------------------------------------------------------------
// Integration test — loads a real .lua file and then runs get_sub_keys on
// the resulting in-memory table. This is the same path the CLI takes when
// the user hits Tab.
// --------------------------------------------------------------------------

class GetSubKeysIntegration : public ::testing::Test {
protected:
  std::filesystem::path tmp_dir;

  void SetUp() override {
    tmp_dir = std::filesystem::temp_directory_path() /
              ("cli_completion_test_" +
               std::to_string(
                   ::testing::UnitTest::GetInstance()->random_seed()));
    std::filesystem::create_directories(tmp_dir);
  }

  void TearDown() override {
    std::error_code ec;
    std::filesystem::remove_all(tmp_dir, ec);
  }

  std::string write_lua(const std::string &name, const std::string &body) {
    auto path = tmp_dir / name;
    std::ofstream os(path);
    os << body;
    os.close();
    return path.string();
  }
};

TEST_F(GetSubKeysIntegration, StartDownlinkConnectionsTestRequestShape) {
  auto path = write_lua("cmd.lua", R"LUA(
    return {
      StartDownlinkConnectionsTestRequest = {
        rate_mbps    = 50,
        duration_sec = 30,
        bidirectional = true,
        device_id    = { "d1", "d2" },
        test_param   = {
          mode        = "burst",
          packet_size = 1500,
        }
      }
    }
  )LUA");

  lua_file engine;
  engine.process_create_luafile(path);

  const auto &file_root = engine.commands().at(path);
  const auto &cmd_entry =
      file_root.members.at("StartDownlinkConnectionsTestRequest");
  const auto &cmd_table =
      *std::get<std::shared_ptr<lua_file::table_type>>(cmd_entry);

  // Empty filter — every parameter the CLI should offer on the first Tab.
  std::vector<std::string> all;
  get_sub_keys(cmd_table, "", "", all);
  EXPECT_TRUE(contains(all, "rate_mbps="));
  EXPECT_TRUE(contains(all, "duration_sec="));
  EXPECT_TRUE(contains(all, "bidirectional="));
  EXPECT_TRUE(contains(all, "device_id="));
  EXPECT_TRUE(contains(all, "test_param."));
  EXPECT_TRUE(contains(all, "test_param.mode="));
  EXPECT_TRUE(contains(all, "test_param.packet_size="));

  // After typing "test_param." — only nested descendants.
  std::vector<std::string> nested;
  get_sub_keys(cmd_table, "", "test_param.", nested);
  EXPECT_TRUE(contains(nested, "test_param.mode="));
  EXPECT_TRUE(contains(nested, "test_param.packet_size="));
  EXPECT_FALSE(contains(nested, "rate_mbps="));
}

} // namespace
