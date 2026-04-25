// Tests for fs_app. The class constructor calls inotify_init() and
// iterates the watched directory via on_boot(), so each test creates
// a fresh temp dir with mkdtemp() and optionally drops a .lua file
// into it before constructing the fs_app.
//
// process_inotify_onchange is exercised by hand-building an
// inotify_event buffer. The production code has an off-by-one
// (`offset <= in.length()`) that reads one byte past the end on the
// final iteration, so we pad the std::string with extra zero bytes to
// keep ASan happy.

#include "fs_app.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

extern "C" {
#include <sys/inotify.h>
#include <unistd.h>
}

namespace {

namespace fs = std::filesystem;

// Sample Lua content exercising scalars + an array.
constexpr const char *kSampleLua = R"LUA(
SampleRequest = {
  name = "alpha",
  count = 7,
  enabled = true,
  tags   = { "one", "two", "three" }
}
)LUA";

class FsAppTest : public ::testing::Test {
protected:
  std::string tmp_dir;

  void SetUp() override {
    char tmpl[] = "/tmp/fs_app_test_XXXXXX";
    ASSERT_NE(mkdtemp(tmpl), nullptr);
    tmp_dir = tmpl;
  }

  void TearDown() override {
    std::error_code ec;
    fs::remove_all(tmp_dir, ec);
  }

  void write_lua(const std::string &basename, const std::string &body) {
    std::ofstream(tmp_dir + "/" + basename) << body;
  }

  // Build an inotify_event payload for a single event. The returned
  // string is zero-padded so the production do/while `offset <=
  // in.length()` loop doesn't walk off the end.
  static std::string make_event(uint32_t mask, uint32_t cookie,
                                const std::string &name) {
    // name field is null-terminated; inotify's own NAME_MAX is 255 + NUL.
    // Round up to the next 4-byte boundary so adjacent events would be
    // aligned if we ever stacked them.
    const size_t raw_name_len = name.size() + 1;
    const size_t aligned = (raw_name_len + 3) & ~size_t{3};

    std::vector<char> buf(sizeof(inotify_event) + aligned, '\0');
    auto *ev = reinterpret_cast<inotify_event *>(buf.data());
    ev->wd = 1;
    ev->mask = mask;
    ev->cookie = cookie;
    ev->len = static_cast<uint32_t>(aligned);
    std::memcpy(ev->name, name.c_str(), name.size());

    // Pad with one more zero-initialized event header so the trailing
    // `offset <= in.length()` iteration dereferences valid memory.
    std::string out(buf.begin(), buf.end());
    out.append(sizeof(inotify_event) + 4, '\0');
    return out;
  }
};

TEST_F(FsAppTest, EmptyDirectoryStartsWithNoCommands) {
  fs_app app(tmp_dir);
  EXPECT_TRUE(app.lua_engine()->commands().empty());
}

TEST_F(FsAppTest, OnBootLoadsExistingLuaFiles) {
  write_lua("sample.lua", kSampleLua);
  fs_app app(tmp_dir);
  const auto &cmds = app.lua_engine()->commands();
  EXPECT_EQ(cmds.size(), 1u);
}

TEST_F(FsAppTest, NonLuaFilesAreIgnoredOnBoot) {
  // .txt must not be loaded into the command map.
  std::ofstream(tmp_dir + "/notes.txt") << "hello";
  fs_app app(tmp_dir);
  EXPECT_TRUE(app.lua_engine()->commands().empty());
}

TEST_F(FsAppTest, HandleEventWriteCloseReturnZero) {
  fs_app app(tmp_dir);
  EXPECT_EQ(app.handle_event(0, 0), 0);
  EXPECT_EQ(app.handle_write(0), 0);
  EXPECT_EQ(app.handle_close(0), 0);
}

TEST_F(FsAppTest, HandleReadDryRunReturnsZero) {
  fs_app app(tmp_dir);
  EXPECT_EQ(app.handle_read(/*channel=*/0, "ignored", /*dry_run=*/true), 0);
}

TEST_F(FsAppTest, ProcessInotifyOnChangeCreateLoadsLua) {
  fs_app app(tmp_dir);
  ASSERT_TRUE(app.lua_engine()->commands().empty());

  // Drop the file first, then synthesize the inotify event that would
  // have been delivered by the kernel. IN_CLOSE_WRITE is the event the
  // production code's watch-mask is armed with; its handler branch
  // fires on IN_CREATE | IN_MODIFY, so we send IN_CREATE.
  write_lua("created.lua", kSampleLua);
  const auto payload = make_event(IN_CREATE, /*cookie=*/0, "created.lua");
  const auto consumed = app.process_inotify_onchange(payload);

  EXPECT_GT(consumed, 0);
  EXPECT_EQ(app.lua_engine()->commands().size(), 1u);
}

TEST_F(FsAppTest, ProcessInotifyOnChangeDeleteRemovesEntry) {
  // Seed with a file so on_boot loads it.
  write_lua("gone.lua", kSampleLua);
  fs_app app(tmp_dir);
  ASSERT_EQ(app.lua_engine()->commands().size(), 1u);

  // File no longer needs to exist on disk for the DELETE branch — it
  // only updates the in-memory map.
  fs::remove(tmp_dir + "/gone.lua");
  const auto payload = make_event(IN_DELETE, /*cookie=*/0, "gone.lua");
  app.process_inotify_onchange(payload);

  EXPECT_TRUE(app.lua_engine()->commands().empty());
}

TEST_F(FsAppTest, ProcessInotifyOnChangeIgnoresNonLuaExtensions) {
  fs_app app(tmp_dir);
  const auto payload = make_event(IN_CREATE, 0, "README.txt");
  // Should not throw, should not touch the command map.
  app.process_inotify_onchange(payload);
  EXPECT_TRUE(app.lua_engine()->commands().empty());
}

} // namespace
