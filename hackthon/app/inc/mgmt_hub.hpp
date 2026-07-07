#ifndef __mgmt_hub_hpp__
#define __mgmt_hub_hpp__

// tNMI mgmt dial-out state: connected DialTcc.Subscribe sessions + correlation.
//
// A device opens the bidi /tnmi.DialTcc/Subscribe stream: it sends DeviceResponse
// up, receives DeviceRequest down. The operator types a CLI command; we build a
// DeviceRequest (CliRequest packed into Any), stamp a random rpc_id, send it on
// every open session, and remember the rpc_id so the matching DeviceResponse can
// be labelled as a reply (vs an unsolicited/proactive push). Single-threaded —
// everything runs on the libevent loop thread, so no locking.

#include "grpc_session.hpp"
#include "dialout/tnmi_dialout.pb.h"

#include <google/protobuf/message.h>

#include <cstdint>
#include <ctime>
#include <map>
#include <random>
#include <string>
#include <vector>

class mgmt_hub {
public:
  struct session_info {
    int id{0};
    std::time_t since{0};
    std::string device;   // device_id last seen on this session ("" until a reply)
    std::string hostname; // from the on-open /system/state probe
    std::string role;     // from the on-open /system/state probe
  };

  static mgmt_hub &instance() {
    static mgmt_hub hub;
    return hub;
  }

  // A device opened Subscribe — track it; returns the session id (for display).
  int add_session(grpc_session *g, std::int32_t sid, std::time_t now) {
    const int id = ++m_next_id;
    m_sessions.push_back({g, sid, id, now});
    return id;
  }

  // Connection closed — drop its session(s). Returns the removed session ids.
  std::vector<int> remove_by_stream(grpc_session *g) {
    std::vector<int> gone;
    for (auto it = m_sessions.begin(); it != m_sessions.end();) {
      if (it->grpc == g) {
        gone.push_back(it->id);
        it = m_sessions.erase(it);
      } else {
        ++it;
      }
    }
    return gone;
  }

  std::size_t size() const { return m_sessions.size(); }
  std::vector<session_info> snapshot() const {
    std::vector<session_info> out;
    out.reserve(m_sessions.size());
    for (const auto &s : m_sessions)
      out.push_back({s.id, s.since, s.device, s.hostname, s.role});
    return out;
  }

  // Record the device_id seen on a session's stream (from a DeviceResponse).
  void note_device(grpc_session *g, std::int32_t sid,
                   const std::string &device_id) {
    if (device_id.empty())
      return;
    for (auto &s : m_sessions)
      if (s.grpc == g && s.sid == sid)
        s.device = device_id;
  }

  // Record hostname/role from the on-open /system/state identity probe.
  void set_identity(grpc_session *g, std::int32_t sid,
                    const std::string &hostname, const std::string &role) {
    for (auto &s : m_sessions)
      if (s.grpc == g && s.sid == sid) {
        if (!hostname.empty()) s.hostname = hostname;
        if (!role.empty()) s.role = role;
      }
  }

  // Pack any operation message into a DeviceRequest{rpc_id, device_id, request}
  // and send it on every open session. `label` is remembered so replies can be
  // correlated back and shown. Returns the rpc_id ("" if no session connected).
  std::string send_request(const google::protobuf::Message &op,
                           const std::string &device_id,
                           const std::string &label) {
    if (m_sessions.empty())
      return "";
    const std::string rpc_id = gen_rpc_id();

    tnmi::DeviceRequest req;
    req.set_rpc_id(rpc_id);
    if (!device_id.empty())
      req.set_device_id(device_id);
    req.mutable_request()->PackFrom(op);

    std::string pb;
    req.SerializeToString(&pb);
    for (auto &s : m_sessions)
      s.grpc->stream_send(s.sid, pb);

    m_pending[rpc_id] = label;
    return rpc_id;
  }

  // Send a fully-built DeviceRequest (e.g. from a .lua file): stamp a fresh
  // rpc_id (overriding any in the message) and send on every session.
  std::string send_device_request(tnmi::DeviceRequest &req,
                                  const std::string &label) {
    if (m_sessions.empty())
      return "";
    const std::string rpc_id = gen_rpc_id();
    req.set_rpc_id(rpc_id);
    std::string pb;
    req.SerializeToString(&pb);
    for (auto &s : m_sessions)
      s.grpc->stream_send(s.sid, pb);
    m_pending[rpc_id] = label;
    return rpc_id;
  }

  // Convenience: build a CliRequest from the sticky settings and send it.
  std::string send_cli(const std::string &cmd,
                       const std::vector<std::string> &args,
                       const std::string &device_id, bool cec_cli, bool json,
                       std::uint64_t timeout_ns) {
    tnmi::DeviceRequest::CliRequest cli;
    cli.set_cmd(cmd);
    for (const auto &a : args)
      cli.add_args(a);
    if (cec_cli)
      cli.set_cec_cli(true);
    if (json)
      cli.set_json(true);
    if (timeout_ns > 0) {
      cli.mutable_timeout()->set_seconds(
          static_cast<std::int64_t>(timeout_ns / 1000000000ULL));
      cli.mutable_timeout()->set_nanos(
          static_cast<std::int32_t>(timeout_ns % 1000000000ULL));
    }
    return send_request(cli, device_id, cmd);
  }

  // Was `rpc_id` a command we sent? Returns the command text, or "" if unknown
  // (an unknown rpc_id ⇒ a proactive/unsolicited push).
  std::string command_for(const std::string &rpc_id) const {
    auto it = m_pending.find(rpc_id);
    return it == m_pending.end() ? std::string() : it->second;
  }

private:
  mgmt_hub() : m_rng(std::random_device{}()) {}

  std::string gen_rpc_id() {
    static const char *hex = "0123456789abcdef";
    std::string s = "r-";
    for (int i = 0; i < 12; ++i)
      s += hex[m_dist(m_rng) & 0xF];
    return s;
  }

  struct sess {
    grpc_session *grpc{nullptr};
    std::int32_t sid{0};
    int id{0};
    std::time_t since{0};
    std::string device;   // last device_id seen on this stream
    std::string hostname; // from /system/state probe
    std::string role;     // from /system/state probe
  };
  std::vector<sess> m_sessions;
  int m_next_id{0};
  std::map<std::string, std::string> m_pending; // rpc_id -> command text
  std::mt19937 m_rng;
  std::uniform_int_distribution<int> m_dist{0, 255};
};

#endif // __mgmt_hub_hpp__
