// Unit tests for http2_session. Two sessions are wired together in memory:
// the client's drain_send_buf() output is fed into the server's recv() and
// vice versa. This lets us verify the full request/response cycle without
// any real sockets or TLS.

#include "http2.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>
#include <vector>

namespace {

// Drain one side and deliver the bytes to the other. Returns bytes piped.
static size_t pipe_once(http2_session &src, http2_session &dst) {
  std::string data = src.drain_send_buf();
  if (data.empty())
    return 0;
  ssize_t consumed =
      dst.recv(reinterpret_cast<const uint8_t *>(data.data()), data.size());
  return consumed > 0 ? static_cast<size_t>(consumed) : 0;
}

// Pump until neither side wants to write (or iteration limit).
static void pump(http2_session &a, http2_session &b, int limit = 32) {
  for (int i = 0; i < limit && (a.want_write() || b.want_write()); ++i) {
    pipe_once(a, b);
    pipe_once(b, a);
  }
}

// -------------------------------------------------------------------------
// Fixture
// -------------------------------------------------------------------------

class Http2SessionTest : public ::testing::Test {
protected:
  http2_session::request last_server_req;
  int32_t last_server_stream{-1};

  http2_session::request last_client_resp;
  int32_t last_client_stream{-1};

  std::unique_ptr<http2_session> srv;
  std::unique_ptr<http2_session> cli;

  void SetUp() override {
    srv = std::make_unique<http2_session>(
        /*server_side=*/true,
        [this](int32_t sid, const http2_session::request &req) {
          last_server_stream = sid;
          last_server_req = req;
        });

    cli = std::make_unique<http2_session>(
        /*server_side=*/false,
        [this](int32_t sid, const http2_session::request &resp) {
          last_client_stream = sid;
          last_client_resp = resp;
        });
  }
};

// -------------------------------------------------------------------------
// Construction
// -------------------------------------------------------------------------

TEST_F(Http2SessionTest, ServerCreatesWithoutThrowing) {
  EXPECT_NE(srv.get(), nullptr);
}

TEST_F(Http2SessionTest, ClientCreatesWithoutThrowing) {
  EXPECT_NE(cli.get(), nullptr);
}

// -------------------------------------------------------------------------
// Initial state
// -------------------------------------------------------------------------

TEST_F(Http2SessionTest, ServerWantsWriteAfterConstruction) {
  // Server queued an initial SETTINGS frame.
  EXPECT_TRUE(srv->want_write());
}

TEST_F(Http2SessionTest, ClientWantsWriteAfterConstruction) {
  // Client queued connection preface + SETTINGS.
  EXPECT_TRUE(cli->want_write());
}

TEST_F(Http2SessionTest, ClientProducesNonEmptyBytesOnFirstDrain) {
  std::string out = cli->drain_send_buf();
  // Must contain at least the 24-byte connection preface.
  EXPECT_GE(out.size(), 24u);
}

// -------------------------------------------------------------------------
// Handshake
// -------------------------------------------------------------------------

TEST_F(Http2SessionTest, HandshakeCompletesWithoutError) {
  pump(*cli, *srv);
  EXPECT_FALSE(cli->want_write());
  EXPECT_FALSE(srv->want_write());
}

TEST_F(Http2SessionTest, BothSidesWantReadAfterHandshake) {
  pump(*cli, *srv);
  EXPECT_TRUE(srv->want_read());
  EXPECT_TRUE(cli->want_read());
}

TEST_F(Http2SessionTest, DrainAfterHandshakeIsEmpty) {
  pump(*cli, *srv);
  EXPECT_TRUE(cli->drain_send_buf().empty());
  EXPECT_TRUE(srv->drain_send_buf().empty());
}

// -------------------------------------------------------------------------
// Request / response round-trip
// -------------------------------------------------------------------------

TEST_F(Http2SessionTest, RequestHandlerFiredAfterGetRequest) {
  pump(*cli, *srv); // complete handshake

  const int32_t sid = cli->submit_request("GET", "/hello", "localhost");
  ASSERT_GT(sid, 0);

  pump(*cli, *srv);

  EXPECT_EQ(last_server_stream, sid);
  EXPECT_EQ(last_server_req.method, "GET");
  EXPECT_EQ(last_server_req.path, "/hello");
  EXPECT_EQ(last_server_req.authority, "localhost");
  EXPECT_EQ(last_server_req.scheme, "https");
}

TEST_F(Http2SessionTest, ResponseHandlerFiredAfterServerReply) {
  pump(*cli, *srv);

  const int32_t sid = cli->submit_request("GET", "/ping", "host");
  ASSERT_GT(sid, 0);
  pump(*cli, *srv); // deliver request to server

  // Server replies 200 with a body.
  srv->submit_response(last_server_stream, 200,
                       {{"content-type", "text/plain"}}, "pong");
  pump(*srv, *cli); // deliver response to client

  EXPECT_EQ(last_client_stream, sid);
  EXPECT_EQ(last_client_resp.status, 200);
  EXPECT_EQ(last_client_resp.body, "pong");
}

TEST_F(Http2SessionTest, RequestWithBodyIsDeliveredToServer) {
  pump(*cli, *srv);

  const int32_t sid =
      cli->submit_request("POST", "/data", "host", "https", {}, "payload");
  ASSERT_GT(sid, 0);
  pump(*cli, *srv);

  EXPECT_EQ(last_server_req.method, "POST");
  EXPECT_EQ(last_server_req.body, "payload");
}

TEST_F(Http2SessionTest, ExtraRequestHeadersAreParsed) {
  pump(*cli, *srv);

  cli->submit_request("GET", "/", "host", "https",
                      {{"x-custom", "value123"}});
  pump(*cli, *srv);

  auto it = last_server_req.headers.find("x-custom");
  ASSERT_NE(it, last_server_req.headers.end());
  EXPECT_EQ(it->second, "value123");
}

TEST_F(Http2SessionTest, MultipleSequentialRequestsAreHandled) {
  pump(*cli, *srv);

  int32_t sid1 = cli->submit_request("GET", "/a", "host");
  ASSERT_GT(sid1, 0);
  pump(*cli, *srv);
  EXPECT_EQ(last_server_req.path, "/a");

  int32_t sid2 = cli->submit_request("GET", "/b", "host");
  ASSERT_GT(sid2, 0);
  pump(*cli, *srv);
  EXPECT_EQ(last_server_req.path, "/b");

  // HTTP/2 stream ids are always odd and strictly increasing for clients.
  EXPECT_GT(sid2, sid1);
}

TEST_F(Http2SessionTest, EmptyBodyResponseHasNoBody) {
  pump(*cli, *srv);

  cli->submit_request("GET", "/", "host");
  pump(*cli, *srv);

  srv->submit_response(last_server_stream, 204, {}, "");
  pump(*srv, *cli);

  EXPECT_EQ(last_client_resp.status, 204);
  EXPECT_TRUE(last_client_resp.body.empty());
}

// -------------------------------------------------------------------------
// Error / edge cases
// -------------------------------------------------------------------------

TEST_F(Http2SessionTest, RecvReturnsNegativeForBadClientMagic) {
  // A server session receiving garbage at the start gets a protocol error.
  http2_session server(/*server_side=*/true);
  const uint8_t junk[] = "not-http2-data";
  ssize_t rc = server.recv(junk, sizeof(junk) - 1);
  EXPECT_LT(rc, 0);
}

TEST_F(Http2SessionTest, DrainSendBufIdempotentWhenEmpty) {
  pump(*cli, *srv);
  // Both sides quiesced — subsequent drains must return empty.
  EXPECT_TRUE(cli->drain_send_buf().empty());
  EXPECT_TRUE(cli->drain_send_buf().empty());
}

} // namespace
