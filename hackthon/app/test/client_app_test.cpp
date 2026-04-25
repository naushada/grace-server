// Tests for connected_client. We pass a nullptr `server*` because the
// handlers under test do not dereference `m_parent`. The fd is supplied
// via socketpair so the underlying bufferevent_socket_new() call in
// evt_io succeeds.

#include "client_app.hpp"

#include <gtest/gtest.h>

extern "C" {
#include <sys/socket.h>
#include <unistd.h>
}

namespace {

class ConnectedClientTest : public ::testing::Test {
protected:
  int sv[2] = {-1, -1};

  void SetUp() override {
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  }

  void TearDown() override {
    if (sv[0] >= 0)
      close(sv[0]);
    if (sv[1] >= 0)
      close(sv[1]);
  }
};

TEST_F(ConnectedClientTest, HandleReadDryRunReturnsZero) {
  connected_client c(sv[0], "127.0.0.1", /*parent=*/nullptr);
  sv[0] = -1;
  EXPECT_EQ(c.handle_read(/*channel=*/0, "hello", /*dry_run=*/true), 0);
}

TEST_F(ConnectedClientTest, HandleReadWetRunReturnsDataLength) {
  connected_client c(sv[0], "127.0.0.1", nullptr);
  sv[0] = -1;

  EXPECT_EQ(c.handle_read(0, "hello", /*dry_run=*/false), 5);
  EXPECT_EQ(c.handle_read(0, "", /*dry_run=*/false), 0);
  EXPECT_EQ(c.handle_read(0, std::string(100, 'x'), /*dry_run=*/false), 100);
}

TEST_F(ConnectedClientTest, OtherHandlersReturnZero) {
  connected_client c(sv[0], "127.0.0.1", nullptr);
  sv[0] = -1;

  EXPECT_EQ(c.handle_event(0, 0), 0);
  EXPECT_EQ(c.handle_write(0), 0);
  EXPECT_EQ(c.handle_close(0), 0);
}

TEST_F(ConnectedClientTest, ParentAccessorExposesStoredPointer) {
  // Cast-addressable placeholder — we never dereference it, only verify
  // that parent() returns the stored reference.
  auto *fake = reinterpret_cast<server *>(0xdeadbeef);
  connected_client c(sv[0], "127.0.0.1", fake);
  sv[0] = -1;
  EXPECT_EQ(&c.parent(), fake);
}

} // namespace
