// Tests for server. The server's listening socket is bound to
// 127.0.0.1:0 so the OS picks a free ephemeral port — this avoids
// port collisions in CI. We never dispatch the event loop, so
// server_accept_cb is not fired; we exercise handle_connect /
// handle_close / handle_accept directly.

#include "client_app.hpp"
#include "server_app.hpp"
#include "tls_config.hpp"

#include <gtest/gtest.h>

extern "C" {
#include <sys/socket.h>
#include <unistd.h>
}

namespace {

class ServerTest : public ::testing::Test {
protected:
  // Ephemeral port lets the OS pick any free port, preventing
  // flakiness when tests run in parallel.
  std::unique_ptr<server> srv;
  int sv[2] = {-1, -1};

  void SetUp() override {
    srv = std::make_unique<server>("127.0.0.1", /*port=*/0);
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  }

  void TearDown() override {
    srv.reset();
    if (sv[0] >= 0)
      close(sv[0]);
    if (sv[1] >= 0)
      close(sv[1]);
  }
};

TEST_F(ServerTest, ClientsMapStartsEmpty) {
  EXPECT_TRUE(srv->clients().empty());
}

TEST_F(ServerTest, HandleAcceptReturnsZero) {
  EXPECT_EQ(srv->handle_accept(/*channel=*/42, "127.0.0.1"), 0);
}

TEST_F(ServerTest, HandleConnectAddsClientToMap) {
  const int channel = sv[0];
  srv->handle_connect(channel, "127.0.0.1");
  sv[0] = -1; // bufferevent owns it now
  EXPECT_EQ(srv->clients().size(), 1u);
  EXPECT_NE(srv->clients().find(channel), srv->clients().end());
}

TEST_F(ServerTest, HandleCloseRemovesClient) {
  const int channel = sv[0];
  srv->handle_connect(channel, "127.0.0.1");
  sv[0] = -1;
  ASSERT_EQ(srv->clients().size(), 1u);

  // erase() returns number removed (1).
  EXPECT_EQ(srv->handle_close(channel), 1);
  EXPECT_TRUE(srv->clients().empty());
}

TEST_F(ServerTest, HandleCloseOnUnknownChannelReturnsZero) {
  // erase() returns 0 when the key isn't present.
  EXPECT_EQ(srv->handle_close(/*channel=*/999), 0);
}

TEST_F(ServerTest, ConnectedClientParentPointsBackToServer) {
  const int channel = sv[0];
  srv->handle_connect(channel, "10.0.0.1");
  sv[0] = -1;

  auto it = srv->clients().find(channel);
  ASSERT_NE(it, srv->clients().end());
  EXPECT_EQ(&it->second->parent(), srv.get());
}

// ---------------------------------------------------------------------------
// TLS server tests — verify that the TLS ctor builds the listener without
// crashing.  We do not run the event loop so no actual TLS handshake occurs;
// the tests just confirm the object lifecycle is correct.
// ---------------------------------------------------------------------------

TEST(ServerTlsTest, PlainTlsConfigDisabledBehavesLikePlainServer) {
  tls_config tls;           // enabled = false by default
  tls.cert_file = "";
  tls.key_file  = "";
  tls.ca_file   = "";
  // TLS disabled → evt_io(host, port, nullptr, listener_tag{}) → plain TCP
  auto srv = std::make_unique<server>("127.0.0.1", 0, tls);
  EXPECT_TRUE(srv->clients().empty());
}

TEST(ServerTlsTest, TlsEnabledWithCertsCreatesServer) {
  // Use the test certificates baked into the source tree.
  // In CI the build runs from /src so relative path works; in-container
  // they are at /app/certs/.  Try both.
  const std::string base =
      []() -> std::string {
        for (auto &p : {"certs", "/app/certs", "../../certs"}) {
          std::string f = std::string(p) + "/server.pem";
          if (::access(f.c_str(), R_OK) == 0) return p;
        }
        return "";
      }();

  if (base.empty()) {
    GTEST_SKIP() << "test certs not found — skipping TLS server ctor test";
  }

  tls_config tls;
  tls.enabled   = true;
  tls.cert_file = base + "/server.pem";
  tls.key_file  = base + "/server.key";
  tls.ca_file   = base + "/ca.pem";

  auto srv = std::make_unique<server>("127.0.0.1", 0, tls);
  EXPECT_TRUE(srv->clients().empty());
}

} // namespace
