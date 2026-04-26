// Tests for connected_client.
//
// The handle_read path now feeds raw bytes into a grpc_session (which wraps
// http2_session internally).  We use a raw http2_session on the client side to
// generate realistic input for the server-side grpc_session.
//
// Dry-run semantics: handle_read(dry_run=true) always returns 0 ("can handle").
// Wet-run semantics: returns bytes-consumed (>0) on valid HTTP/2, or <0 on error.

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

  // Build the bytes a real HTTP/2 (or gRPC) client sends at connection start:
  // PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n + client SETTINGS frame.
  // A grpc_session server accepts these because it wraps http2_session.
  static std::string valid_http2_preface() {
    http2_session client(/*server_side=*/false);
    return client.drain_send_buf();
  }
};

// ---------------------------------------------------------------------------
// Dry-run gate
// ---------------------------------------------------------------------------

TEST_F(ConnectedClientTest, HandleReadDryRunReturnsZero) {
  connected_client c(sv[0], "127.0.0.1", /*parent=*/nullptr);
  sv[0] = -1;
  EXPECT_EQ(c.handle_read(/*channel=*/0, "any data", /*dry_run=*/true), 0);
}

// ---------------------------------------------------------------------------
// Wet-run: valid HTTP/2 / gRPC connection preface
// ---------------------------------------------------------------------------

TEST_F(ConnectedClientTest, HandleReadWetRunWithValidHttp2ReturnsPositive) {
  connected_client c(sv[0], "127.0.0.1", nullptr);
  sv[0] = -1;

  const auto preface = valid_http2_preface();
  // grpc_session wraps http2_session; a valid HTTP/2 preface is consumed fine.
  EXPECT_GT(c.handle_read(0, preface, /*dry_run=*/false), 0);
}

// ---------------------------------------------------------------------------
// Wet-run: garbage bytes trigger an HTTP/2 protocol error
// ---------------------------------------------------------------------------

TEST_F(ConnectedClientTest, HandleReadWetRunWithGarbageReturnsNegative) {
  connected_client c(sv[0], "127.0.0.1", nullptr);
  sv[0] = -1;
  // "hello" is not a valid HTTP/2 client connection preface.
  EXPECT_LT(c.handle_read(0, "hello", /*dry_run=*/false), 0);
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

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
