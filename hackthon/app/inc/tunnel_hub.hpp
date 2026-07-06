#ifndef __tunnel_hub_hpp__
#define __tunnel_hub_hpp__

// Registry of targets registered over the grpctunnel Register RPC.
//
// A tunnel client (device) dials the server, opens Register, and sends
// Target{op=ADD, target, target_type} for each service it fronts. That target
// is recorded here with the owning Register stream (grpc_session + stream id) so
// the server can later request a data tunnel to it (Session{tag,target} — that
// is Increment B). Single-threaded (libevent loop) — no lock.

#include "grpc_session.hpp"

#include <cstdint>
#include <ctime>
#include <string>
#include <unordered_map>
#include <vector>

class tunnel_hub {
public:
  // Snapshot row for the monitor TUI.
  struct target_info {
    std::string target;
    std::string type;   // target_type, e.g. "GNMI_GNOI"
    std::time_t since{0};
  };

  static tunnel_hub &instance() {
    static tunnel_hub hub;
    return hub;
  }

  void add_target(const std::string &target, const std::string &type,
                  grpc_session *g, std::int32_t sid, std::time_t now) {
    m_targets[target] = reg{g, sid, type, now};
  }
  void remove_target(const std::string &target) { m_targets.erase(target); }

  // Drop every target owned by a Register stream that closed; return their ids.
  std::vector<std::string> remove_by_stream(grpc_session *g) {
    std::vector<std::string> gone;
    for (auto it = m_targets.begin(); it != m_targets.end();) {
      if (it->second.grpc == g) {
        gone.push_back(it->first);
        it = m_targets.erase(it);
      } else {
        ++it;
      }
    }
    return gone;
  }

  bool has(const std::string &target) const {
    return m_targets.find(target) != m_targets.end();
  }
  std::size_t size() const { return m_targets.size(); }

  std::vector<target_info> snapshot() const {
    std::vector<target_info> out;
    out.reserve(m_targets.size());
    for (const auto &kv : m_targets)
      out.push_back({kv.first, kv.second.type, kv.second.since});
    return out;
  }

private:
  tunnel_hub() = default;
  struct reg {
    grpc_session *grpc{nullptr};
    std::int32_t stream_id{0};
    std::string type;
    std::time_t since{0};
  };
  std::unordered_map<std::string, reg> m_targets;
};

#endif // __tunnel_hub_hpp__
