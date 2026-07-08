// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#ifndef __sub_hub_hpp__
#define __sub_hub_hpp__

// sub_hub — a server-wide registry of active gNMI Subscribe streams.
//
// gNMI Subscribe and Set arrive on *different* connections (each is its own
// connected_client / grpc_session). To turn a Set into on-change telemetry, the
// Set handler publishes the request here; the hub fans it out as a
// SubscribeResponse notification to every subscriber whose subscribed paths
// cover a changed path (subtree match). Single-threaded (shared event loop), so
// no locking is required.

#include "grpc_session.hpp"
#include "gnmi_util.hpp"

#include "gnmi/gnmi.pb.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class sub_hub {
public:
  static sub_hub &instance() {
    static sub_hub h;
    return h;
  }

  // Register a STREAM subscription. `owner` is an opaque identity (the
  // connected_client) used to deregister on close. Empty `paths` = match all.
  void add(const void *owner, grpc_session *grpc, std::int32_t stream_id,
           std::vector<std::string> paths) {
    m_subs.push_back({owner, grpc, stream_id, std::move(paths)});
  }

  // Deregister every subscription belonging to `owner` (call on close).
  void remove(const void *owner) {
    m_subs.erase(std::remove_if(m_subs.begin(), m_subs.end(),
                                [&](const sub &s) { return s.owner == owner; }),
                 m_subs.end());
  }

  // Fan out a Set's operations to matching subscribers as on-change updates.
  void publish(const gnmi::SetRequest &req) {
    if (m_subs.empty())
      return;
    const std::int64_t ts =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();

    for (const auto &s : m_subs) {
      gnmi::SubscribeResponse resp;
      auto *n = resp.mutable_update();
      n->set_timestamp(ts);
      bool any = false;
      for (const auto &u : req.update())
        if (matches(s.paths, u.path())) {
          *n->add_update() = u;
          any = true;
        }
      for (const auto &u : req.replace())
        if (matches(s.paths, u.path())) {
          *n->add_update() = u;
          any = true;
        }
      for (const auto &d : req.delete_())
        if (matches(s.paths, d)) {
          *n->add_delete_() = d;
          any = true;
        }
      if (!any)
        continue;
      std::string out;
      resp.SerializeToString(&out);
      s.grpc->stream_send(s.stream_id, out);
    }
  }

private:
  struct sub {
    const void *owner;
    grpc_session *grpc;
    std::int32_t stream_id;
    std::vector<std::string> paths;
  };

  // A subscription to path P covers a change at U when U == P or U is under P
  // (subtree). Empty subscription list = match all.
  static bool matches(const std::vector<std::string> &paths,
                      const gnmi::Path &p) {
    if (paths.empty())
      return true;
    const std::string u = gnmi_util::path_to_string(p);
    for (const auto &sp : paths) {
      if (sp == "/" || u == sp)
        return true;
      if (u.rfind(sp + "/", 0) == 0) // u is strictly under sp
        return true;
    }
    return false;
  }

  std::vector<sub> m_subs;
};

#endif // __sub_hub_hpp__
