// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

// Unit tests for openvpn_client and openvpn_server subsystems.
//
// ParseHelpersTest    — token_after / looks_like_ipv4 / parse_routing_row
// RoutingTableDiffTest — management-interface VIP diff algorithm (server)
// OpenvpnClientVipTest — VIP extraction across all log-line formats (client)
// OpenvpnClientTunnelTest — tunnel-up detection and full sequence
// MqttSubCfgTest      — mqtt_sub_cfg struct defaults and assignment
// OpenvpnServerArgsTest — argv composed for the spawned openvpn binary

#include "openvpn_client.hpp"
#include "openvpn_parse.hpp"
#include "openvpn_server.hpp"
#include "tls_config.hpp"
#include "vpn_types.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <string>
#include <vector>

// ==========================================================================
// Testable subclass — exposes the protected parse_line without forking.
// ==========================================================================

class TestableClient : public openvpn_client {
public:
  TestableClient() : openvpn_client() {}
  void feed(const std::string &line) { parse_line(line); }
};

// ==========================================================================
// ParseHelpersTest — token_after, looks_like_ipv4, parse_routing_row
// ==========================================================================

class ParseHelpersTest : public ::testing::Test {};

TEST_F(ParseHelpersTest, TokenAfterExtractsToken) {
  EXPECT_EQ(token_after("ip addr add dev tun0 local 10.8.0.6 peer", "local "), "10.8.0.6");
}

TEST_F(ParseHelpersTest, TokenAfterReturnsEmptyWhenMissing) {
  EXPECT_TRUE(token_after("some unrelated line", "local ").empty());
}

TEST_F(ParseHelpersTest, TokenAfterTrimsTrailingComma) {
  // PUSH_REPLY format: "ifconfig 10.8.0.6 10.8.0.5,peer-id 0"
  EXPECT_EQ(token_after("PUSH_REPLY,ifconfig 10.8.0.6 10.8.0.5,peer-id 0", "ifconfig "), "10.8.0.6");
}

TEST_F(ParseHelpersTest, TokenAfterTrimsTrailingColon) {
  EXPECT_EQ(token_after("addr: 192.168.1.1:50000", "addr: "), "192.168.1.1");
}

TEST_F(ParseHelpersTest, TokenAfterHandlesTokenAtEnd) {
  EXPECT_EQ(token_after("prefix=10.0.0.1", "prefix="), "10.0.0.1");
}

TEST_F(ParseHelpersTest, LooksLikeIpv4AcceptsValidIp) {
  EXPECT_TRUE(looks_like_ipv4("10.8.0.6"));
  EXPECT_TRUE(looks_like_ipv4("192.168.100.200"));
  EXPECT_TRUE(looks_like_ipv4("1.2.3.4"));
}

TEST_F(ParseHelpersTest, LooksLikeIpv4RejectsTooShort) {
  EXPECT_FALSE(looks_like_ipv4("1.2.3"));   // only 5 chars
  EXPECT_FALSE(looks_like_ipv4(""));
}

TEST_F(ParseHelpersTest, LooksLikeIpv4RejectsNoDot) {
  EXPECT_FALSE(looks_like_ipv4("12345678"));
}

TEST_F(ParseHelpersTest, LooksLikeIpv4RejectsLetters) {
  EXPECT_FALSE(looks_like_ipv4("10.8.0.abc"));
  EXPECT_FALSE(looks_like_ipv4("hostname"));
}

TEST_F(ParseHelpersTest, ParseRoutingRowExtractsVip) {
  EXPECT_EQ(parse_routing_row("10.8.0.6,client1,1.2.3.4:50000,2024-01-01"), "10.8.0.6");
  EXPECT_EQ(parse_routing_row("192.168.10.5,peer2,5.6.7.8:12345,2024-01-02"), "192.168.10.5");
}

TEST_F(ParseHelpersTest, ParseRoutingRowSkipsHeaderRow) {
  // Header starts with 'V' — must be skipped.
  EXPECT_TRUE(parse_routing_row("Virtual Address,Common Name,Real Address,Last Ref").empty());
}

TEST_F(ParseHelpersTest, ParseRoutingRowReturnsEmptyForNoComma) {
  EXPECT_TRUE(parse_routing_row("nocolumn").empty());
}

TEST_F(ParseHelpersTest, ParseRoutingRowReturnsEmptyForNonIpFirst) {
  // First field has no dot → not a VIP
  EXPECT_TRUE(parse_routing_row("clientname,foo,bar").empty());
}

// ==========================================================================
// RoutingTableDiffTest — mirrors openvpn_server mgmt_io::parse_mgmt_line
// ==========================================================================

class RoutingTableDiffTest : public ::testing::Test {
protected:
  routing_table_diff diff;

  // Feed a complete "status 2" response and return the aggregate result.
  routing_table_diff::result feed_status(const std::vector<std::string> &rows) {
    routing_table_diff::result agg;
    for (const auto &l : {"TITLE", "OpenVPN CLIENT LIST"}) {
      auto r = diff.feed(l);
      agg.connected.insert(agg.connected.end(), r.connected.begin(), r.connected.end());
      agg.disconnected.insert(agg.disconnected.end(), r.disconnected.begin(), r.disconnected.end());
    }
    auto r = diff.feed("ROUTING TABLE");
    for (const auto &row : rows) {
      auto rr = diff.feed(row);
      agg.connected.insert(agg.connected.end(), rr.connected.begin(), rr.connected.end());
      agg.disconnected.insert(agg.disconnected.end(), rr.disconnected.begin(), rr.disconnected.end());
    }
    auto end_r = diff.feed("END");
    agg.connected.insert(agg.connected.end(), end_r.connected.begin(), end_r.connected.end());
    agg.disconnected.insert(agg.disconnected.end(), end_r.disconnected.begin(), end_r.disconnected.end());
    return agg;
  }
};

TEST_F(RoutingTableDiffTest, SingleClientConnects) {
  const auto r = feed_status({"10.8.0.6,client1,1.2.3.4:1000,now"});
  ASSERT_EQ(r.connected.size(), 1u);
  EXPECT_EQ(r.connected[0], "10.8.0.6");
  EXPECT_TRUE(r.disconnected.empty());
}

TEST_F(RoutingTableDiffTest, ClientDisconnects) {
  feed_status({"10.8.0.6,client1,1.2.3.4:1000,now"});
  const auto r = feed_status({});  // empty table → client gone
  EXPECT_TRUE(r.connected.empty());
  ASSERT_EQ(r.disconnected.size(), 1u);
  EXPECT_EQ(r.disconnected[0], "10.8.0.6");
}

TEST_F(RoutingTableDiffTest, MultipleClientsTracked) {
  const auto r = feed_status({
    "10.8.0.6,c1,1.2.3.4:1000,now",
    "10.8.0.10,c2,5.6.7.8:2000,now",
  });
  EXPECT_EQ(r.connected.size(), 2u);
  EXPECT_TRUE(r.disconnected.empty());
}

TEST_F(RoutingTableDiffTest, NewClientAddedWhileOthersRemain) {
  feed_status({"10.8.0.6,c1,1.2.3.4:1000,now"});
  const auto r = feed_status({
    "10.8.0.6,c1,1.2.3.4:1000,now",
    "10.8.0.10,c2,5.6.7.8:2000,now",
  });
  ASSERT_EQ(r.connected.size(), 1u);
  EXPECT_EQ(r.connected[0], "10.8.0.10");
  EXPECT_TRUE(r.disconnected.empty());
}

TEST_F(RoutingTableDiffTest, ClientReplacedByAnother) {
  feed_status({"10.8.0.6,c1,1.2.3.4:1000,now"});
  const auto r = feed_status({"10.8.0.10,c2,5.6.7.8:2000,now"});
  ASSERT_EQ(r.connected.size(), 1u);
  EXPECT_EQ(r.connected[0], "10.8.0.10");
  ASSERT_EQ(r.disconnected.size(), 1u);
  EXPECT_EQ(r.disconnected[0], "10.8.0.6");
}

TEST_F(RoutingTableDiffTest, HeaderRowIsSkipped) {
  // "Virtual Address,..." starts with 'V' and must not be treated as a VIP.
  const auto r = feed_status({"Virtual Address,Common Name,Real Address,Last Ref"});
  EXPECT_TRUE(r.connected.empty());
  EXPECT_TRUE(r.disconnected.empty());
}

TEST_F(RoutingTableDiffTest, LogLinesIgnored) {
  const auto r = diff.feed(">LOG:1234567890,I,OpenVPN started");
  EXPECT_TRUE(r.connected.empty());
  EXPECT_TRUE(r.disconnected.empty());
}

TEST_F(RoutingTableDiffTest, LinesOutsideTableSectionIgnored) {
  // Feeding a VIP-like line when not inside a ROUTING TABLE section
  // must not change state.
  const auto r = diff.feed("10.8.0.6,c1,1.2.3.4:1000,now");
  EXPECT_TRUE(r.connected.empty());
  EXPECT_TRUE(r.disconnected.empty());
  EXPECT_TRUE(diff.active.empty());
}

TEST_F(RoutingTableDiffTest, GlobalStatsAlsoTerminatesTable) {
  diff.feed("ROUTING TABLE");
  diff.feed("10.8.0.6,c1,1.2.3.4:1000,now");
  const auto r = diff.feed("GLOBAL STATS");
  ASSERT_EQ(r.connected.size(), 1u);
  EXPECT_EQ(r.connected[0], "10.8.0.6");
}

TEST_F(RoutingTableDiffTest, SecondPollWithSameClientsProducesNoDiff) {
  feed_status({"10.8.0.6,c1,1.2.3.4:1000,now"});
  const auto r = feed_status({"10.8.0.6,c1,1.2.3.4:1000,now"});
  EXPECT_TRUE(r.connected.empty());
  EXPECT_TRUE(r.disconnected.empty());
}

// ==========================================================================
// OpenvpnClientVipTest — parse_line VIP extraction
// ==========================================================================

class OpenvpnClientVipTest : public ::testing::Test {
protected:
  TestableClient client;
};

TEST_F(OpenvpnClientVipTest, ModernAddrAddLocal) {
  client.feed("ip addr add dev tun0 local 10.8.0.6 peer 10.8.0.5");
  EXPECT_EQ(client.assigned_ip(), "10.8.0.6");
}

TEST_F(OpenvpnClientVipTest, PushReplyIfconfig) {
  client.feed("PUSH_REPLY,ifconfig 10.8.0.6 10.8.0.5,peer-id 0,auth-token ...");
  EXPECT_EQ(client.assigned_ip(), "10.8.0.6");
}

TEST_F(OpenvpnClientVipTest, LegacyIfconfigTun) {
  client.feed("ifconfig tun0 10.8.0.6 10.8.0.1");
  EXPECT_EQ(client.assigned_ip(), "10.8.0.6");
}

TEST_F(OpenvpnClientVipTest, IfconfigLocal) {
  client.feed("ifconfig_local=10.8.0.3");
  EXPECT_EQ(client.assigned_ip(), "10.8.0.3");
}

TEST_F(OpenvpnClientVipTest, NetAddrPtpV4Add) {
  client.feed("net_addr_ptp_v4_add: 10.8.0.6 peer 10.8.0.5 dev tun0");
  EXPECT_EQ(client.assigned_ip(), "10.8.0.6");
}

TEST_F(OpenvpnClientVipTest, UnrelatedLineProducesNoVip) {
  client.feed("OpenVPN 2.6.4 x86_64-pc-linux-gnu [SSL (OpenSSL)] [LZO] built on ...");
  EXPECT_TRUE(client.assigned_ip().empty());
}

TEST_F(OpenvpnClientVipTest, VipNotOverwrittenBySecondLine) {
  client.feed("ip addr add dev tun0 local 10.8.0.6 peer 10.8.0.5");
  ASSERT_EQ(client.assigned_ip(), "10.8.0.6");
  // A second VIP-bearing line (different address) must not overwrite the first.
  client.feed("ifconfig_local=10.8.0.99");
  EXPECT_EQ(client.assigned_ip(), "10.8.0.6");
}

TEST_F(OpenvpnClientVipTest, IfconfigLineWithoutDeviceSkipped) {
  // "ifconfig" without a device name in the right position should not match legacy path.
  client.feed("Some prefix ifconfig notalegit format");
  EXPECT_TRUE(client.assigned_ip().empty());
}

// ==========================================================================
// OpenvpnClientTunnelTest — tunnel-up detection
// ==========================================================================

class OpenvpnClientTunnelTest : public ::testing::Test {
protected:
  TestableClient client;
};

TEST_F(OpenvpnClientTunnelTest, NotUpInitially) {
  EXPECT_FALSE(client.tunnel_up());
  EXPECT_TRUE(client.assigned_ip().empty());
}

TEST_F(OpenvpnClientTunnelTest, TunnelUpAfterInitSeq) {
  client.feed("Initialization Sequence Completed");
  EXPECT_TRUE(client.tunnel_up());
}

TEST_F(OpenvpnClientTunnelTest, UnrelatedLineDoesNotSetTunnelUp) {
  client.feed("Some other log line");
  EXPECT_FALSE(client.tunnel_up());
}

TEST_F(OpenvpnClientTunnelTest, FullSequenceVipThenTunnelUp) {
  // Typical sequence: VIP extracted first, then tunnel-up fires.
  client.feed("ip addr add dev tun0 local 10.8.0.6 peer 10.8.0.5");
  EXPECT_FALSE(client.tunnel_up());
  EXPECT_EQ(client.assigned_ip(), "10.8.0.6");

  client.feed("Initialization Sequence Completed");
  EXPECT_TRUE(client.tunnel_up());
  EXPECT_EQ(client.assigned_ip(), "10.8.0.6");
}

TEST_F(OpenvpnClientTunnelTest, TunnelUpBeforeVipStillSetsFlag) {
  // Tunnel-up line before VIP (unusual but valid): flag is set.
  client.feed("Initialization Sequence Completed");
  EXPECT_TRUE(client.tunnel_up());
  EXPECT_TRUE(client.assigned_ip().empty());

  // VIP arrives late — still recorded.
  client.feed("ifconfig_local=10.8.0.7");
  EXPECT_EQ(client.assigned_ip(), "10.8.0.7");
}

// ==========================================================================
// MqttSubCfgTest — mqtt_sub_cfg struct
// ==========================================================================

class MqttSubCfgTest : public ::testing::Test {};

TEST_F(MqttSubCfgTest, DefaultConstruction) {
  mqtt_sub_cfg cfg;
  EXPECT_FALSE(cfg.enabled);
  EXPECT_EQ(cfg.host, "localhost");
  EXPECT_EQ(cfg.port, 1883u);
  EXPECT_EQ(cfg.gnmi_port, 58989u);
}

TEST_F(MqttSubCfgTest, FieldAssignment) {
  mqtt_sub_cfg cfg;
  cfg.enabled   = true;
  cfg.host      = "broker.example.com";
  cfg.port      = 8883;
  cfg.gnmi_port = 9339;
  EXPECT_TRUE(cfg.enabled);
  EXPECT_EQ(cfg.host, "broker.example.com");
  EXPECT_EQ(cfg.port, 8883u);
  EXPECT_EQ(cfg.gnmi_port, 9339u);
}

TEST_F(MqttSubCfgTest, EnabledDisabledToggle) {
  mqtt_sub_cfg cfg;
  EXPECT_FALSE(cfg.enabled);
  cfg.enabled = true;
  EXPECT_TRUE(cfg.enabled);
  cfg.enabled = false;
  EXPECT_FALSE(cfg.enabled);
}

// ==========================================================================
// OpenvpnServerArgsTest — verifies the argv we hand to the openvpn binary
// includes the client-CN pin and the right cert/DH wiring per TLS mode.
// ==========================================================================

namespace {
// Returns the value that follows `flag` in args, or empty string if absent.
std::string value_after(const std::vector<std::string> &args,
                         const std::string &flag) {
  for (size_t i = 0; i + 1 < args.size(); ++i)
    if (args[i] == flag) return args[i + 1];
  return {};
}
// True iff the three-token sequence appears consecutively in args.
bool contains_triple(const std::vector<std::string> &args,
                      const std::string &a,
                      const std::string &b,
                      const std::string &c) {
  for (size_t i = 0; i + 2 < args.size(); ++i)
    if (args[i] == a && args[i + 1] == b && args[i + 2] == c) return true;
  return false;
}
} // namespace

class OpenvpnServerArgsTest : public ::testing::Test {};

TEST_F(OpenvpnServerArgsTest, CnPinPresentWhenTlsOff) {
  tls_config tls; // disabled
  auto args = openvpn_server::build_args(1194, tls, 7505);
  EXPECT_TRUE(contains_triple(args, "--verify-x509-name", "vpn-client", "name"))
      << "client CN pin must apply even in non-TLS branch";
}

TEST_F(OpenvpnServerArgsTest, CnPinPresentWhenTlsOn) {
  tls_config tls;
  tls.enabled   = true;
  tls.ca_file   = "/app/certs/ca.pem";
  tls.cert_file = "/app/certs/server.pem";
  tls.key_file  = "/app/certs/server.key";
  auto args = openvpn_server::build_args(1194, tls, 7505);
  EXPECT_TRUE(contains_triple(args, "--verify-x509-name", "vpn-client", "name"));
}

TEST_F(OpenvpnServerArgsTest, TlsBranchUsesDhNone) {
  tls_config tls;
  tls.enabled   = true;
  tls.ca_file   = "/app/certs/ca.pem";
  tls.cert_file = "/app/certs/server.pem";
  tls.key_file  = "/app/certs/server.key";
  auto args = openvpn_server::build_args(1194, tls, 7505);
  EXPECT_EQ(value_after(args, "--dh"), "none")
      << "TLS branch must use ECDH (--dh none), not a static dh.pem path";
  EXPECT_EQ(value_after(args, "--ca"),   "/app/certs/ca.pem");
  EXPECT_EQ(value_after(args, "--cert"), "/app/certs/server.pem");
  EXPECT_EQ(value_after(args, "--key"),  "/app/certs/server.key");
}

TEST_F(OpenvpnServerArgsTest, NonTlsBranchFallsBackToBakedCertsAndNullCipher) {
  tls_config tls; // disabled
  auto args = openvpn_server::build_args(1194, tls, 7505);
  EXPECT_EQ(value_after(args, "--ca"),           "/app/certs/ca.pem");
  EXPECT_EQ(value_after(args, "--cert"),         "/app/certs/server.pem");
  EXPECT_EQ(value_after(args, "--key"),          "/app/certs/server.key");
  EXPECT_EQ(value_after(args, "--dh"),           "none");
  EXPECT_EQ(value_after(args, "--data-ciphers"), "none");
  EXPECT_EQ(value_after(args, "--auth"),         "none");
}

TEST_F(OpenvpnServerArgsTest, VpnAndMgmtPortsAreThreadedThrough) {
  tls_config tls;
  auto args = openvpn_server::build_args(2194, tls, 9505);
  EXPECT_EQ(value_after(args, "--port"), "2194");
  // --management uses two args: "127.0.0.1" then the port string.
  for (size_t i = 0; i + 2 < args.size(); ++i) {
    if (args[i] == "--management") {
      EXPECT_EQ(args[i + 1], "127.0.0.1");
      EXPECT_EQ(args[i + 2], "9505");
      return;
    }
  }
  FAIL() << "--management flag not found in argv";
}
