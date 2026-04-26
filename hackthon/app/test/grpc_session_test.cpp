// Unit tests for grpc_session.
//
// Two sessions are wired in memory: a grpc_session (server) and a raw
// http2_session (client).  The client submits gRPC-framed requests and
// receives responses; the server dispatches to registered handlers.

#include "grpc_session.hpp"
#include "http2.hpp"

#include <cstdint>
#include <gtest/gtest.h>
#include <string>

namespace {

// ---------------------------------------------------------------------------
// In-memory loopback helpers
// ---------------------------------------------------------------------------

// Deliver all bytes queued by src's tx callback to dst's recv().
static void deliver(const std::string &data, http2_session &dst) {
  if (!data.empty())
    dst.recv(reinterpret_cast<const uint8_t *>(data.data()), data.size());
}

// Pump until neither side has pending output.
static void pump(grpc_session &srv, http2_session &cli,
                 std::string &srv_to_cli_buf, int limit = 32) {
  for (int i = 0; i < limit && (srv.want_write() || cli.want_write()); ++i) {
    // client → server
    std::string c2s = cli.drain_send_buf();
    if (!c2s.empty())
      srv.recv(reinterpret_cast<const uint8_t *>(c2s.data()), c2s.size());

    // server → client (srv_to_cli_buf collected via tx callback)
    deliver(srv_to_cli_buf, cli);
    srv_to_cli_buf.clear();
  }
}

// ---------------------------------------------------------------------------
// Test fixture
// ---------------------------------------------------------------------------

class GrpcSessionTest : public ::testing::Test {
protected:
  // Bytes the grpc_session wants to send to the "network"
  std::string srv_buf;

  std::unique_ptr<grpc_session> srv;
  std::unique_ptr<http2_session> cli;

  http2_session::request last_resp;
  int32_t last_stream{-1};

  void SetUp() override {
    srv_buf.clear();

    srv = std::make_unique<grpc_session>([this](const char *d, size_t n) {
      srv_buf.append(d, n);
    });

    cli = std::make_unique<http2_session>(
        /*server_side=*/false,
        [this](int32_t sid, const http2_session::request &r) {
          last_stream = sid;
          last_resp = r;
        });
  }

  // Complete the HTTP/2 handshake.
  void handshake() { pump(*srv, *cli, srv_buf); }

  // Submit a gRPC unary request from the client side and pump until done.
  int32_t send_request(const std::string &path,
                       const std::string &request_pb) {
    const std::string framed = grpc_session::encode_frame(request_pb);
    const int32_t sid = cli->submit_request(
        "POST", path, "localhost", "http",
        {{"content-type", "application/grpc+proto"},
         {"te", "trailers"}},
        framed);
    pump(*srv, *cli, srv_buf);
    return sid;
  }
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

TEST_F(GrpcSessionTest, ServerCreatesWithoutThrowing) {
  EXPECT_NE(srv.get(), nullptr);
}

TEST_F(GrpcSessionTest, ServerSendsSettingsOnConstruction) {
  // The grpc_session ctor should have triggered a SETTINGS flush.
  EXPECT_FALSE(srv_buf.empty());
}

// ---------------------------------------------------------------------------
// Handshake
// ---------------------------------------------------------------------------

TEST_F(GrpcSessionTest, HandshakeCompletesCleanly) {
  handshake();
  EXPECT_FALSE(srv->want_write());
  EXPECT_FALSE(cli->want_write());
}

// ---------------------------------------------------------------------------
// Frame encoding / decoding
// ---------------------------------------------------------------------------

TEST_F(GrpcSessionTest, EncodeFrameHasCorrectHeader) {
  const std::string payload = "hello";
  const std::string frame = grpc_session::encode_frame(payload);
  ASSERT_EQ(frame.size(), 10u); // 5 header + 5 payload
  EXPECT_EQ(static_cast<uint8_t>(frame[0]), 0u); // not compressed
  EXPECT_EQ(static_cast<uint8_t>(frame[4]), 5u); // length LSB
  EXPECT_EQ(frame.substr(5), payload);
}

TEST_F(GrpcSessionTest, DecodeFrameRoundTrips) {
  const std::string payload = "world";
  std::string buf = grpc_session::encode_frame(payload);
  const std::string decoded = grpc_session::decode_frame(buf);
  EXPECT_EQ(decoded, payload);
  EXPECT_TRUE(buf.empty()); // buffer fully consumed
}

TEST_F(GrpcSessionTest, DecodeFrameReturnsEmptyOnIncompleteData) {
  std::string buf("\x00\x00\x00\x00\x05he", 7); // header says 5 bytes, only 2 present
  const std::string result = grpc_session::decode_frame(buf);
  EXPECT_TRUE(result.empty());
  EXPECT_EQ(buf.size(), 7u); // unchanged
}

TEST_F(GrpcSessionTest, DecodeFrameReturnsEmptyOnTooShortBuffer) {
  std::string buf = "\x00\x00"; // less than 5-byte header
  const std::string result = grpc_session::decode_frame(buf);
  EXPECT_TRUE(result.empty());
}

// ---------------------------------------------------------------------------
// Unary RPC dispatch
// ---------------------------------------------------------------------------

TEST_F(GrpcSessionTest, UnaryHandlerInvokedWithCorrectPayload) {
  std::string received_pb;
  srv->register_unary("/mypackage.MyService/Echo",
                       [&](const std::string &req_pb) -> std::pair<int, std::string> {
                         received_pb = req_pb;
                         return {0, "resp"};
                       });

  handshake();
  send_request("/mypackage.MyService/Echo", "request_data");

  EXPECT_EQ(received_pb, "request_data");
}

TEST_F(GrpcSessionTest, UnaryResponseBodyDeliveredToClient) {
  srv->register_unary("/svc/Method",
                       [](const std::string &) -> std::pair<int, std::string> {
                         return {0, "response_pb"};
                       });

  handshake();
  send_request("/svc/Method", "req");

  // Client should have received the response; body is gRPC-framed
  std::string body_copy = last_resp.body;
  const std::string decoded = grpc_session::decode_frame(body_copy);
  EXPECT_EQ(decoded, "response_pb");
}

TEST_F(GrpcSessionTest, UnaryResponseHasGrpcStatusOK) {
  srv->register_unary("/svc/Method",
                       [](const std::string &) -> std::pair<int, std::string> {
                         return {0, ""};
                       });

  handshake();
  send_request("/svc/Method", "");

  // grpc-status should be in the trailing headers captured by on_header
  auto it = last_resp.headers.find("grpc-status");
  ASSERT_NE(it, last_resp.headers.end());
  EXPECT_EQ(it->second, "0");
}

TEST_F(GrpcSessionTest, UnaryHandlerCanReturnNonZeroGrpcStatus) {
  srv->register_unary("/svc/Fail",
                       [](const std::string &) -> std::pair<int, std::string> {
                         return {3, ""}; // 3 = INVALID_ARGUMENT
                       });

  handshake();
  send_request("/svc/Fail", "");

  auto it = last_resp.headers.find("grpc-status");
  ASSERT_NE(it, last_resp.headers.end());
  EXPECT_EQ(it->second, "3");
}

TEST_F(GrpcSessionTest, UnimplementedMethodReturnsStatus12) {
  handshake();
  send_request("/unknown/Method", "");

  auto it = last_resp.headers.find("grpc-status");
  ASSERT_NE(it, last_resp.headers.end());
  EXPECT_EQ(it->second, "12"); // UNIMPLEMENTED
}

TEST_F(GrpcSessionTest, WrongContentTypeReturnsHttp415) {
  handshake();

  // Send without gRPC content-type
  cli->submit_request("POST", "/svc/Method", "localhost", "http",
                      {{"content-type", "application/json"}}, "body");
  pump(*srv, *cli, srv_buf);

  EXPECT_EQ(last_resp.status, 415);
}

TEST_F(GrpcSessionTest, MultipleSequentialRpcsAreHandled) {
  int call_count = 0;
  srv->register_unary("/svc/Inc",
                       [&](const std::string &) -> std::pair<int, std::string> {
                         ++call_count;
                         return {0, ""};
                       });

  handshake();
  send_request("/svc/Inc", "a");
  send_request("/svc/Inc", "b");

  EXPECT_EQ(call_count, 2);
}

TEST_F(GrpcSessionTest, ResponseHttp2StatusIs200ForValidRpc) {
  srv->register_unary("/svc/Method",
                       [](const std::string &) -> std::pair<int, std::string> {
                         return {0, "ok"};
                       });

  handshake();
  send_request("/svc/Method", "");

  EXPECT_EQ(last_resp.status, 200);
}

} // namespace
