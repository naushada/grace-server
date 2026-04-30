#ifndef __openvpn_parse_hpp__
#define __openvpn_parse_hpp__

#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

// Shared parsing helpers used by openvpn_client and tested directly.

// Return the next whitespace-delimited token immediately after `key` in
// `line`, trimming any trailing non-IPv4 characters (comma, colon, etc.).
inline std::string token_after(const std::string &line, const std::string &key) {
  const auto pos = line.find(key);
  if (pos == std::string::npos) return {};
  std::istringstream ss(line.substr(pos + key.size()));
  std::string tok;
  ss >> tok;
  const auto end = tok.find_first_not_of("0123456789.");
  return tok.substr(0, end);
}

// Return true if s looks like a dotted-decimal IPv4 address (no letters,
// at least one dot, minimum 7 chars like "1.2.3.4").
inline bool looks_like_ipv4(const std::string &s) {
  return s.size() >= 7 && s.find('.') != std::string::npos &&
         s.find_first_not_of("0123456789.") == std::string::npos;
}

// Extract the virtual-IP from one "status 2" ROUTING TABLE row.
// Format: "Virtual Address,Common Name,Real Address,Last Ref"
// The header row starts with 'V' and is skipped; returns empty on any error.
inline std::string parse_routing_row(const std::string &line) {
  if (line.empty() || line[0] == 'V') return {};
  const auto comma = line.find(',');
  if (comma == std::string::npos) return {};
  const std::string vip = line.substr(0, comma);
  if (vip.find('.') == std::string::npos) return {};
  return vip;
}

// ---------------------------------------------------------------------------
// routing_table_diff — mirrors the VIP-tracking logic in
// openvpn_server's mgmt_io::parse_mgmt_line.  Used only by unit tests.
// ---------------------------------------------------------------------------
struct routing_table_diff {
  struct result {
    std::vector<std::string> connected;
    std::vector<std::string> disconnected;
  };

  bool                             in_table{false};
  std::unordered_set<std::string>  polled;   // VIPs seen in current status 2
  std::unordered_set<std::string>  active;   // VIPs from the last completed poll

  // Feed one management-interface line; returns any connect/disconnect events.
  result feed(const std::string &line) {
    result r;

    if (line.rfind(">LOG:", 0) == 0) return r;

    if (line == "ROUTING TABLE") {
      in_table = true;
      polled.clear();
      return r;
    }

    if (line == "GLOBAL STATS" || line == "END") {
      if (in_table) {
        in_table = false;
        for (const auto &v : polled)
          if (!active.count(v)) r.connected.push_back(v);
        for (const auto &v : active)
          if (!polled.count(v)) r.disconnected.push_back(v);
        active = polled;
      }
      return r;
    }

    if (in_table) {
      const std::string vip = parse_routing_row(line);
      if (!vip.empty()) polled.insert(vip);
    }
    return r;
  }
};

#endif // __openvpn_parse_hpp__
