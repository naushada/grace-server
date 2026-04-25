// Tests for connected_client. We pass a nullptr `server*` because the
// handlers under test do not dereference `m_parent`. The fd is supplied
// via socketpair so the underlying bufferevent_socket_new() call in
// evt_io succeeds.
//
// handle_read now feeds raw bytes into an HTTP/2 session (nghttp2), so
// the return-value semantics changed: positive = bytes consumed, negative
// = nghttp2 protocol error.

#include "client_app.hpp"
#include "http2.hpp"

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

  // Build the bytes a real HTTP/2 client would send at connection start
  // (connection preface + SETTINGS frame). These are valid input for the
  // server-side HTTP/2 session inside connected_client.
  static std::string valid_http2_preface() {
    http2_session client(/*server_side=*/false);
    return client.drain_send_buf();
  }
};

TEST_F(ConnectedClientTest, HandleReadDryRunReturnsZero) {
  connected_client c(sv[0], "127.0.0.1", /*parent=*/nullptr);
  sv[0] = -1;
  EXPECT_EQ(c.handle_read(/*channel=*/0, "any data", /*dry_run=*/true), 0);
}

TEST_F(ConnectedClientTest, HandleReadWetRunWithValidHttp2ReturnsPositive) {
  connected_client c(sv[0], "127.0.0.1", nullptr);
  sv[0] = -1;

  const auto preface = valid_http2_preface();
  // Feed the valid client connection preface into the server-side session.
  EXPECT_GT(c.handle_read(0, preface, /*dry_run=*/false), 0);
}

TEST_F(ConnectedClientTest, HandleReadWetRunWithGarbageReturnsNegative) {
  connected_client c(sv[0], "127.0.0.1", nullptr);
  sv[0] = -1;
  // "hello" is not a valid HTTP/2 client connection preface.
  EXPECT_LT(c.handle_read(0, "hello", /*dry_run=*/false), 0);
}

TEST_F(ConnectedClientTest, HandleReadEmptyDataIsHandledGracefully) {
  connected_client c(sv[0], "127.0.0.1", nullptr);
  sv[0] = -1;
  // Empty input — nghttp2_session_mem_recv returns 0, not an error.
  EXPECT_GE(c.handle_read(0, "", /*dry_run=*/false), 0);
}

TEST_F(ConnectedClientTest, OtherHandlersReturnZero) {
  connected_client c(sv[0], "127.0.0.1", nullptr);
  sv[0] = -1;

  EXPECT_EQ(c.handle_event(0, 0), 0);
  EXPECT_EQ(c.handle_write(0), 0);
  EXPECT_EQ(c.handle_close(0), 0);
}

TEST_F(ConnectedClientTest, ParentAccessorExposesStoredPointer) {
  auto *fake = reinterpret_cast<server *>(0xdeadbeef);
  connected_client c(sv[0], "127.0.0.1", fake);
  sv[0] = -1;
  EXPECT_EQ(&c.parent(), fake);
}

} // namespace
