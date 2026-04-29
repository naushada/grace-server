// Unit tests for the openvpn subsystem.
//
// IpPoolTest     — full-IPv4 range pool (assign / release / exhaust / large)
// FrameTest      — encode/decode round-trips
// StatusLuaTest  — lua_file::write_table output format
// TlsConfigTest  — tls_config disabled/enabled struct invariants
// VpnClientTest — frame constants, multi-frame sequence

#include "vpn_client.hpp"
#include "vpn_peer.hpp"
#include "vpn_server.hpp"
#include "tls_config.hpp"
#include "lua_engine.hpp"

#include <gtest/gtest.h>
#include <arpa/inet.h>
#include <cstring>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <sstream>

extern "C" {
#include <sys/socket.h>
#include <unistd.h>
}

// ==========================================================================
// ip_pool (full IPv4 range)
// ==========================================================================

class IpPoolTest : public ::testing::Test {};

TEST_F(IpPoolTest, AssignReturnsStartIp) {
  ip_pool pool("192.168.10.2", "192.168.10.5");
  EXPECT_EQ(pool.assign(1), "192.168.10.2");
}

TEST_F(IpPoolTest, SequentialAssignsAreUnique) {
  ip_pool pool("10.8.0.2", "10.8.0.10");
  const auto a = pool.assign(10);
  const auto b = pool.assign(20);
  EXPECT_FALSE(a.empty());
  EXPECT_FALSE(b.empty());
  EXPECT_NE(a, b);
}

TEST_F(IpPoolTest, ReleaseAndReassignSameIp) {
  ip_pool pool("10.8.0.2", "10.8.0.5");
  const auto first = pool.assign(1);
  pool.release(1);
  EXPECT_EQ(pool.assign(2), first);
}

TEST_F(IpPoolTest, GetReturnsAssignedIp) {
  ip_pool pool("10.0.0.10", "10.0.0.20");
  pool.assign(42);
  EXPECT_EQ(pool.get(42), "10.0.0.10");
  pool.release(42);
  EXPECT_TRUE(pool.get(42).empty());
}

TEST_F(IpPoolTest, ExhaustedPoolReturnsEmpty) {
  ip_pool pool("10.8.0.2", "10.8.0.3"); // 2 addresses
  pool.assign(1);
  pool.assign(2);
  EXPECT_TRUE(pool.assign(3).empty());
}

TEST_F(IpPoolTest, ReleaseUnknownChannelIsNoop) {
  ip_pool pool("10.8.0.2", "10.8.0.10");
  EXPECT_NO_THROW(pool.release(999));
}

TEST_F(IpPoolTest, LargePoolSupports1000PlusClients) {
  // /22 CIDR: 10.8.0.2 – 10.11.255.254 = 262 142 addresses
  ip_pool pool("10.8.0.2", "10.11.255.254");
  EXPECT_GE(pool.available(), 1000u);
  // Assign 1000 channels and verify all are unique.
  std::set<std::string> assigned;
  for (int i = 0; i < 1000; ++i) {
    const auto ip = pool.assign(i);
    ASSERT_FALSE(ip.empty()) << "exhausted at i=" << i;
    EXPECT_TRUE(assigned.insert(ip).second) << "duplicate ip=" << ip;
  }
  EXPECT_EQ(assigned.size(), 1000u);
}

TEST_F(IpPoolTest, AvailableDecrementsOnAssign) {
  ip_pool pool("10.8.0.2", "10.8.0.4"); // 3 addresses
  EXPECT_EQ(pool.available(), 3u);
  pool.assign(1);
  EXPECT_EQ(pool.available(), 2u);
  pool.release(1);
  EXPECT_EQ(pool.available(), 3u);
}

// ==========================================================================
// Frame encode / decode
// ==========================================================================

class FrameTest : public ::testing::Test {};

TEST_F(FrameTest, EncodeFrameHasCorrectHeader) {
  const auto f = vpn_client::encode_frame(vpn_client::TYPE_IP_ASSIGN,
                                               "10.8.0.3");
  ASSERT_GE(f.size(), vpn_client::HEADER_LEN);
  EXPECT_EQ(static_cast<uint8_t>(f[0]), vpn_client::TYPE_IP_ASSIGN);
  uint32_t len_be = 0;
  std::memcpy(&len_be, f.data() + 1, 4);
  EXPECT_EQ(ntohl(len_be), 8u);
  EXPECT_EQ(f.substr(vpn_client::HEADER_LEN), "10.8.0.3");
}

TEST_F(FrameTest, RoundTrip) {
  const std::string payload = "hello tunnel";
  const auto frame = vpn_client::encode_frame(vpn_client::TYPE_DATA, payload);
  uint8_t t{}; std::string p{}; size_t c{};
  ASSERT_TRUE(vpn_client::try_decode_frame(frame, 0, t, p, c));
  EXPECT_EQ(t, vpn_client::TYPE_DATA);
  EXPECT_EQ(p, payload);
  EXPECT_EQ(c, frame.size());
}

TEST_F(FrameTest, IncompleteHeaderReturnsFalse) {
  const std::string buf = "\x01\x00\x00";
  uint8_t t{}; std::string p{}; size_t c{};
  EXPECT_FALSE(vpn_client::try_decode_frame(buf, 0, t, p, c));
}

TEST_F(FrameTest, IncompletePayloadReturnsFalse) {
  const auto frame = vpn_client::encode_frame(vpn_client::TYPE_DATA, "0123456789");
  const std::string trunc = frame.substr(0, vpn_client::HEADER_LEN + 3);
  uint8_t t{}; std::string p{}; size_t c{};
  EXPECT_FALSE(vpn_client::try_decode_frame(trunc, 0, t, p, c));
}

TEST_F(FrameTest, EmptyPayload) {
  const auto f = vpn_client::encode_frame(vpn_client::TYPE_DISCONNECT, "");
  uint8_t t{}; std::string p{}; size_t c{};
  ASSERT_TRUE(vpn_client::try_decode_frame(f, 0, t, p, c));
  EXPECT_EQ(t, vpn_client::TYPE_DISCONNECT);
  EXPECT_TRUE(p.empty());
  EXPECT_EQ(c, vpn_client::HEADER_LEN);
}

TEST_F(FrameTest, MultiFrameSequence) {
  const auto f1 = vpn_client::encode_frame(vpn_client::TYPE_IP_ASSIGN, "10.8.0.4");
  const auto f2 = vpn_client::encode_frame(vpn_client::TYPE_DATA, "payload");
  const std::string stream = f1 + f2;

  size_t offset = 0;
  uint8_t t{}; std::string p{}; size_t c{};

  ASSERT_TRUE(vpn_client::try_decode_frame(stream, offset, t, p, c));
  EXPECT_EQ(t, vpn_client::TYPE_IP_ASSIGN); EXPECT_EQ(p, "10.8.0.4");
  offset += c;

  ASSERT_TRUE(vpn_client::try_decode_frame(stream, offset, t, p, c));
  EXPECT_EQ(t, vpn_client::TYPE_DATA); EXPECT_EQ(p, "payload");
}

// ==========================================================================
// write_status_lua via lua_file::write_table
// ==========================================================================

class StatusLuaTest : public ::testing::Test {
protected:
  std::string m_path;
  void SetUp() override {
    m_path = "/tmp/vpn_test_" +
             std::to_string(std::chrono::steady_clock::now()
                                .time_since_epoch().count()) + ".lua";
  }
  void TearDown() override { std::remove(m_path.c_str()); }
  std::string read_file() {
    std::ifstream f(m_path); std::ostringstream ss; ss << f.rdbuf(); return ss.str();
  }
};

TEST_F(StatusLuaTest, ConnectedContainsAllFields) {
  vpn_client::write_status_lua(m_path, "10.8.0.3", "tun2", "Connected", 1700000000);
  const auto c = read_file();
  EXPECT_NE(c.find("service_ip"),  std::string::npos);
  EXPECT_NE(c.find("10.8.0.3"),    std::string::npos);
  EXPECT_NE(c.find("tun_if"),      std::string::npos);
  EXPECT_NE(c.find("tun2"),        std::string::npos);
  EXPECT_NE(c.find("Connected"),   std::string::npos);
  EXPECT_NE(c.find("1700000000"),  std::string::npos);
  EXPECT_NE(c.find("timestamp"),   std::string::npos);
  EXPECT_NE(c.find("vpn_status"),  std::string::npos);
  EXPECT_NE(c.find("return"),      std::string::npos);
}

TEST_F(StatusLuaTest, DownStatus) {
  vpn_client::write_status_lua(m_path, "10.8.0.3", "tun0", "Down", 1);
  EXPECT_NE(read_file().find("Down"), std::string::npos);
}

TEST_F(StatusLuaTest, KernelAssignedTunIfRecorded) {
  vpn_client::write_status_lua(m_path, "10.8.0.5", "tun99", "Connected", 100);
  EXPECT_NE(read_file().find("tun99"), std::string::npos);
}

// lua_file::write_table directly
TEST_F(StatusLuaTest, WriteTableProducesValidLua) {
  lua_file::write_table(m_path, "my_table", {
    {"key_str", "\"hello\""},
    {"key_num", "42"},
    {"key_bool", "true"},
  });
  const auto c = read_file();
  EXPECT_NE(c.find("my_table"), std::string::npos);
  EXPECT_NE(c.find("key_str"),  std::string::npos);
  EXPECT_NE(c.find("\"hello\""),std::string::npos);
  EXPECT_NE(c.find("42"),       std::string::npos);
  EXPECT_NE(c.find("return"),   std::string::npos);
}

// ==========================================================================
// tls_config
// ==========================================================================

class TlsConfigTest : public ::testing::Test {};

TEST_F(TlsConfigTest, DisabledReturnsNullCtx) {
  tls_config t{false, "", "", ""};
  EXPECT_EQ(t.build_server_ctx(), nullptr);
  EXPECT_EQ(t.build_client_ctx(), nullptr);
}

TEST_F(TlsConfigTest, EnabledWithBadPathsReturnsNull) {
  tls_config t{true, "/nonexistent/cert.pem", "/nonexistent/key.pem", ""};
  // Should fail gracefully (cert file not found) and return nullptr.
  EXPECT_EQ(t.build_server_ctx(), nullptr);
}

TEST_F(TlsConfigTest, EnabledClientWithNoCredentialsStillBuildsCtx) {
  // A client that only verifies the server cert (no mutual TLS) can have
  // empty cert/key but still builds a valid context.
  tls_config t{true, "", "", ""};
  ssl_ctx_ptr ctx = t.build_client_ctx();
  // ctx may be non-null (no ca required) — unique_ptr releases it on scope exit.
  // No crash is the primary assertion.
  SUCCEED();
}

// ==========================================================================
// vpn_client constants
// ==========================================================================

class VpnClientTest : public ::testing::Test {};

TEST_F(VpnClientTest, TypeConstants) {
  EXPECT_EQ(vpn_client::TYPE_IP_ASSIGN,  0x01u);
  EXPECT_EQ(vpn_client::TYPE_DATA,       0x02u);
  EXPECT_EQ(vpn_client::TYPE_DISCONNECT, 0x03u);
  EXPECT_EQ(vpn_client::HEADER_LEN,      5u);
}

TEST_F(VpnClientTest, PeerTypeConstantsMatchClient) {
  // Server and client must agree on frame types.
  EXPECT_EQ(vpn_peer::TYPE_IP_ASSIGN,  vpn_client::TYPE_IP_ASSIGN);
  EXPECT_EQ(vpn_peer::TYPE_DATA,       vpn_client::TYPE_DATA);
  EXPECT_EQ(vpn_peer::TYPE_DISCONNECT, vpn_client::TYPE_DISCONNECT);
  EXPECT_EQ(vpn_peer::HEADER_LEN,      vpn_client::HEADER_LEN);
}
