// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#ifndef __client_app_cpp__
#define __client_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"
#include "gnmi_util.hpp"
#include "mgmt_hub.hpp"
#include "server_app.hpp"
#include "sub_hub.hpp"
#include "tunnel_hub.hpp"
#include "update_sink.hpp"

// Generated protobuf headers (produced by protoc at build time under
// ${CMAKE_BINARY_DIR}/app/proto_gen/).
#include "gnmi/gnmi.pb.h"
#include "tunnel/tunnel.pb.h"
#include "dialout/tnmi_dialout.pb.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iostream>

// Emit a tunnel event to the console AND the update_sink (so the monitor TUI can
// render it). In TUI mode std::cout is redirected to a logfile; in headless it
// is the log and the sink has no subscriber.
static void tunnel_log(const std::string &line) {
  std::cout << line << "\n";
  update_sink::instance().emit(line);
}

// Format a gNMI notification timestamp (nanoseconds since the Unix epoch) as a
// human-readable UTC instant with millisecond precision.
static std::string format_ns_timestamp(std::int64_t ts_ns) {
  if (ts_ns <= 0)
    return "no-timestamp";
  const std::time_t secs = static_cast<std::time_t>(ts_ns / 1000000000LL);
  const long ms = static_cast<long>((ts_ns % 1000000000LL) / 1000000LL);
  std::tm tm_utc{};
  gmtime_r(&secs, &tm_utc);
  char date[32];
  std::strftime(date, sizeof(date), "%Y-%m-%dT%H:%M:%S", &tm_utc);
  char out[48];
  std::snprintf(out, sizeof(out), "%s.%03ldZ", date, ms);
  return out;
}

// ---------------------------------------------------------------------------
// libevent → HTTP/2 → gRPC hook
// ---------------------------------------------------------------------------
// Flow:
//   libevent fires client_read_cb
//     → connected_client::handle_read          (this function)
//       → grpc_session::recv                   (HTTP/2 frame decode)
//         → on_request callback                (gRPC framing decode)
//           → registered unary handler         (proto deserialize / process)
//             → send_unary_response            (proto serialize + gRPC frame)
//               → http2_session::submit_*      (HTTP/2 encode)
//                 → raw_tx callback → tx()     (back into libevent send buffer)
// ---------------------------------------------------------------------------

std::int32_t connected_client::handle_read(const std::int32_t & /*channel*/,
                                           const std::string &data,
                                           const bool &dry_run) {
  // Dry-run: caller asks "can you handle this?" — always yes for HTTP/2.
  if (dry_run)
    return 0;

  // Feed raw socket bytes into the gRPC/HTTP2 stack.
  // grpc_session::recv() internally calls flush() so any response frames
  // produced by the handlers are written back to the socket automatically.
  const ssize_t consumed = m_grpc->recv(
      reinterpret_cast<const uint8_t *>(data.data()), data.size());

  if (consumed < 0) {
    std::cerr << "Fn:" << __func__ << ":" << __LINE__
              << " grpc/http2 recv error: " << consumed << std::endl;
  }

  return static_cast<std::int32_t>(consumed);
}

// ---------------------------------------------------------------------------
// gNMI RPC handlers
// ---------------------------------------------------------------------------

void connected_client::register_gnmi_handlers() {
  // ----- Capabilities -------------------------------------------------------
  // Returns the set of capabilities supported by the target (encodings,
  // supported models, etc.).  We return an empty CapabilityResponse for now;
  // populate SupportedModels / SupportedEncodings as the application grows.
  m_grpc->register_unary(
      "/gnmi.gNMI/Capabilities",
      [](const std::string &req_pb) -> std::pair<int, std::string> {
        gnmi::CapabilityRequest req;
        if (!req.ParseFromString(req_pb)) {
          std::cerr << "[Capabilities] failed to parse request\n";
          return {3, ""}; // 3 = INVALID_ARGUMENT
        }

        gnmi::CapabilityResponse resp;
        // Add supported encodings
        resp.add_supported_encodings(gnmi::JSON);
        resp.add_supported_encodings(gnmi::PROTO);

        std::string out;
        resp.SerializeToString(&out);
        return {0, out};
      });

  // ----- Get ----------------------------------------------------------------
  // Retrieves one or more paths from the data tree.  The stub below echoes the
  // requested paths back in the notification; replace with real data-store
  // lookups as needed.
  m_grpc->register_unary(
      "/gnmi.gNMI/Get",
      [](const std::string &req_pb) -> std::pair<int, std::string> {
        gnmi::GetRequest req;
        if (!req.ParseFromString(req_pb)) {
          std::cerr << "[Get] failed to parse request\n";
          return {3, ""};
        }

        // role is carried in prefix.target(); default to VIEWER if absent.
        const std::string &role = req.prefix().target();
        std::cout << "[Get] role=" << (role.empty() ? "VIEWER" : role)
                  << " path_count=" << req.path_size() << "\n";

        gnmi::GetResponse resp;
        // For each requested path add an empty Notification so the caller
        // gets a well-formed response.
        for (int i = 0; i < req.path_size(); ++i) {
          auto *notif = resp.add_notification();
          *notif->mutable_prefix() = req.prefix();
          // Timestamp in nanoseconds since Unix epoch.  A real implementation
          // would populate Update messages with actual leaf values.
        }

        std::string out;
        resp.SerializeToString(&out);
        return {0, out};
      });

  // ----- Set ----------------------------------------------------------------
  // Applies a set of updates/replaces/deletes to the data tree.
  m_grpc->register_unary(
      "/gnmi.gNMI/Set",
      [](const std::string &req_pb) -> std::pair<int, std::string> {
        gnmi::SetRequest req;
        if (!req.ParseFromString(req_pb)) {
          std::cerr << "[Set] failed to parse request\n";
          return {3, ""};
        }

        // RBAC: only ADMIN may perform Set.  role is in prefix.target();
        // absent or any value other than "ADMIN" is treated as VIEWER.
        const std::string &role = req.prefix().target();
        if (role != "ADMIN") {
          std::cerr << "[Set] PERMISSION_DENIED role="
                    << (role.empty() ? "VIEWER(default)" : role) << "\n";
          return {7, "PERMISSION_DENIED: ADMIN role required for Set"};
        }

        std::cout << "[Set] role=ADMIN update_count=" << req.update_size()
                  << " replace_count=" << req.replace_size()
                  << " delete_count=" << req.delete__size() << "\n";

        gnmi::SetResponse resp;
        // Reflect back each operation as OK, and forward it to any registered
        // update_sink so a UI (gnmi_peer TUI) can render what the remote peer
        // pushed. The sink is a no-op when nothing is registered.
        for (int i = 0; i < req.update_size(); ++i) {
          auto *r = resp.add_response();
          *r->mutable_path() = req.update(i).path();
          r->set_op(gnmi::UpdateResult::UPDATE);
          update_sink::instance().emit(gnmi_util::op_to_json(
              "UPDATE", req.update(i).path(), &req.update(i).val()));
        }
        for (int i = 0; i < req.replace_size(); ++i) {
          auto *r = resp.add_response();
          *r->mutable_path() = req.replace(i).path();
          r->set_op(gnmi::UpdateResult::REPLACE);
          update_sink::instance().emit(gnmi_util::op_to_json(
              "REPLACE", req.replace(i).path(), &req.replace(i).val()));
        }
        for (int i = 0; i < req.delete__size(); ++i) {
          auto *r = resp.add_response();
          *r->mutable_path() = req.delete_(i);
          r->set_op(gnmi::UpdateResult::DELETE);
          update_sink::instance().emit(
              gnmi_util::op_to_json("DELETE", req.delete_(i), nullptr));
        }

        // On-change telemetry: fan this Set out to any matching Subscribe
        // streams (possibly on other connections) via the server-wide hub.
        sub_hub::instance().publish(req);

        std::string out;
        resp.SerializeToString(&out);
        return {0, out};
      });

  // ----- Subscribe ----------------------------------------------------------
  // Server-streaming: the client sends one SubscriptionList; we stream
  // SubscribeResponse notifications back (STREAM samples on a timer; ONCE sends
  // a single batch). Values are synthetic (a monotonic sample counter) since
  // this target has no real datastore.
  m_grpc->register_server_stream(
      "/gnmi.gNMI/Subscribe",
      [this](std::int32_t sid, const std::string &req_pb) {
        start_subscription(sid, req_pb);
      });

  // ----- DialTcc / IsAlive (dial-out liveness) ------------------------------
  // Devices dial out to their controller and probe liveness with a
  // unary /tnmi.DialTcc/IsAlive before they begin pushing telemetry. This is
  // NOT a gNMI method: grace-server otherwise only speaks gnmi.gNMI, so an
  // unregistered path returns UNIMPLEMENTED (12), the device concludes the peer
  // is dead, and it never streams updates.
  //
  // We answer OK (grpc-status 0) with an empty response body, which satisfies a
  // liveness check. We deliberately do not parse the request: the tnmi.DialTcc
  // .proto is not part of this tree, and an empty message is a valid response
  // for any proto3 message type. If the device requires populated response
  // fields, add the real DialTcc proto to app/idl and build the response here.
  m_grpc->register_unary(
      "/tnmi.DialTcc/IsAlive",
      [](const std::string &req_pb) -> std::pair<int, std::string> {
        std::cout << "[IsAlive] DialTcc liveness probe (" << req_pb.size()
                  << " req bytes) -> OK\n";
        return {0, ""};
      });

  // ----- DialTcc / PushSubscriptionUpdates (client-streaming) ---------------
  // After IsAlive the device opens this stream and pushes telemetry messages
  // continuously without ever sending END_STREAM. Per tnmi_dialout.proto the
  // RPC is:
  //   rpc PushSubscriptionUpdates(stream gnmi.SubscribeResponse) returns (UpdateAck)
  // so each streamed message is a standard gnmi.SubscribeResponse — the type we
  // already compile. Decode it and render with the shared JSON helper, emitting
  // through update_sink so it surfaces as a [remote] line (headless) or in the
  // TUI's bottom pane.
  m_grpc->register_client_stream(
      "/tnmi.DialTcc/PushSubscriptionUpdates",
      [](std::int32_t sid, const std::string &msg_pb) {
        gnmi::SubscribeResponse resp;
        if (!resp.ParseFromString(msg_pb)) {
          std::cerr << "[PushSub] stream=" << sid << " parse failed ("
                    << msg_pb.size() << " bytes)\n";
          return;
        }

        // sync_response marks the end of the initial state dump.
        if (resp.response_case() == gnmi::SubscribeResponse::kSyncResponse) {
          update_sink::instance().emit("── sync ──");
          return;
        }
        if (resp.response_case() != gnmi::SubscribeResponse::kUpdate)
          return;

        // Compact rendering so a 200+-leaf notification doesn't wall the screen:
        // one header line (stream · ts · prefix · counts) then the leaves packed
        // as "relpath=val" tokens into wrapped, indented lines (values truncated).
        // Full path = prefix + relpath; prefix is on the header, so leaves show
        // the short relative path. Goes to both stdout and the TUI sink.
        const gnmi::Notification &n = resp.update();
        const std::string prefix = gnmi_util::path_to_string(n.prefix());
        const std::string ts = format_ns_timestamp(n.timestamp());

        auto out = [](const std::string &s) {
          std::cout << s << "\n";
          update_sink::instance().emit(s);
        };

        out("[PushSub] s" + std::to_string(sid) + " " + ts + " " + prefix +
            "  " + std::to_string(n.update_size()) + "u " +
            std::to_string(n.delete__size()) + "d");

        // Pack tokens into lines no wider than kWidth (indented by 4).
        constexpr std::size_t kWidth = 118;
        constexpr std::size_t kValMax = 28; // truncate long values (e.g. messages)
        std::string line = "    ";
        auto pack = [&](const std::string &tok) {
          if (line.size() > 4 && line.size() + tok.size() + 2 > kWidth) {
            out(line);
            line = "    ";
          }
          line += tok + "  ";
        };
        for (const auto &u : n.update()) {
          std::string v = gnmi_util::typed_value_to_json(u.val());
          if (v.size() > kValMax)
            v = v.substr(0, kValMax) + "..";
          pack(gnmi_util::path_to_string(u.path()) + "=" + v);
        }
        for (int i = 0; i < n.delete__size(); ++i)
          pack("-" + gnmi_util::path_to_string(n.delete_(i)));
        if (line.size() > 4)
          out(line);
      });

  // ----- grpctunnel Register (server side, increment A) ---------------------
  // A tunnel client (device) dials in and opens Register (bidi). It sends
  // Target{op=ADD, target, target_type} for each service it fronts; we record
  // the target and ack. Session/Tunnel data-plane setup is increment B.
  m_grpc->register_bidi_stream(
      "/grpctunnel.Tunnel/Register",
      [](std::int32_t sid) {
        tunnel_log("[reg] Register stream opened (stream=" +
                   std::to_string(sid) + ")");
      },
      [this](std::int32_t sid, const std::string &msg_pb) {
        grpctunnel::RegisterOp op;
        if (!op.ParseFromString(msg_pb)) {
          tunnel_log("[reg] stream=" + std::to_string(sid) +
                     " unparsable RegisterOp");
          return;
        }
        switch (op.Registration_case()) {
        case grpctunnel::RegisterOp::kTarget: {
          const grpctunnel::Target &t = op.target();
          if (t.op() == grpctunnel::Target::ADD) {
            tunnel_hub::instance().add_target(t.target(), t.target_type(),
                                              m_grpc.get(), sid,
                                              std::time(nullptr));
            tunnel_log("[reg] +target '" + t.target() + "' (" +
                       t.target_type() + ") — " +
                       std::to_string(tunnel_hub::instance().size()) + " total");
            // If no local listener fronts this target, tell the operator exactly
            // what to add to the tunnel config (and to restart) so gNMI can reach
            // it. The device's published name must be copied verbatim.
            if (tunnel_hub::instance().listener_port(t.target()) == 0) {
              const std::uint16_t p = tunnel_hub::instance().suggest_port();
              tunnel_log("[reg]   \xE2\x9A\xA0 no local port maps to this target."); // ⚠
              tunnel_log("[reg]   Add it to the tunnel config (host docs/tunnel.lua"
                         " -> /app/tunnel.lua):");
              tunnel_log("[reg]       listeners = { [\"" + std::to_string(p) +
                         "\"] = \"" + t.target() + "\" }");
              tunnel_log("[reg]   then restart:  ./service.sh restart");
            }
          } else if (t.op() == grpctunnel::Target::REMOVE) {
            tunnel_hub::instance().remove_target(t.target());
            tunnel_log("[reg] -target '" + t.target() + "'");
          }
          // Ack: echo the Target with accept=true.
          grpctunnel::RegisterOp ack;
          grpctunnel::Target *at = ack.mutable_target();
          at->set_op(t.op());
          at->set_target(t.target());
          at->set_target_type(t.target_type());
          at->set_accept(true);
          std::string out;
          ack.SerializeToString(&out);
          m_grpc->stream_send(sid, out);
          break;
        }
        case grpctunnel::RegisterOp::kSubscription: {
          // A subscriber wants to learn about targets — ack it (increment A).
          grpctunnel::RegisterOp ack;
          grpctunnel::Subscription *s = ack.mutable_subscription();
          s->set_op(op.subscription().op());
          s->set_target_type(op.subscription().target_type());
          s->set_accept(true);
          std::string out;
          ack.SerializeToString(&out);
          m_grpc->stream_send(sid, out);
          tunnel_log("[reg] subscription ack (type='" +
                     op.subscription().target_type() + "')");
          break;
        }
        case grpctunnel::RegisterOp::kSession:
          // Client acks a Session we requested; the data stream follows on the
          // Tunnel RPC below.
          tunnel_log("[reg] session tag=" +
                     std::to_string(op.session().tag()) +
                     (op.session().accept() ? " accept" : ""));
          break;
        default:
          break;
        }
      });

  // ----- grpctunnel Tunnel (data plane, increment B) ------------------------
  // The client opens a Tunnel(stream Data) RPC per session. Data carries the tag
  // (set on every frame); pair the stream to the bridge and relay bytes to/from
  // the operator socket. Data{close} tears the bridge down.
  m_grpc->register_bidi_stream(
      "/grpctunnel.Tunnel/Tunnel",
      [](std::int32_t sid) {
        tunnel_log("[tun] Tunnel stream opened (stream=" +
                   std::to_string(sid) + ")");
      },
      [this](std::int32_t sid, const std::string &msg_pb) {
        grpctunnel::Data d;
        if (!d.ParseFromString(msg_pb))
          return;
        const std::int32_t tag = d.tag();
        tunnel_hub::instance().pair_tunnel(tag, m_grpc.get(), sid);
        if (d.close()) {
          tunnel_hub::instance().close_from_device(tag);
          return;
        }
        if (!d.data().empty())
          tunnel_hub::instance().from_device(tag, d.data());
      });

  // ----- tNMI mgmt dial-out: DialTcc.Subscribe (bidi) -----------------------
  // Device streams DeviceResponse UP (command results + proactive/unsolicited
  // pushes); the operator's typed commands go DOWN as DeviceRequest (sent via
  // mgmt_hub). Track the session so commands can reach it; render each response,
  // correlating rpc_id back to the command that produced it.
  m_grpc->register_bidi_stream(
      "/tnmi.DialTcc/Subscribe",
      [this](std::int32_t sid) {
        const int id = mgmt_hub::instance().add_session(m_grpc.get(), sid,
                                                        std::time(nullptr));
        tunnel_log("[mgmt] session #" + std::to_string(id) +
                   " opened (stream=" + std::to_string(sid) + ")");
        // Probe identity for the prompt/banner: gNMI Get /system/state.
        gnmi::GetRequest g;
        *g.add_path() = gnmi_util::parse_yang_path("/system/state");
        g.set_encoding(gnmi::JSON);
        tnmi::DeviceRequest probe;
        probe.set_rpc_id("__mgmt_probe__");
        probe.mutable_request()->PackFrom(g);
        std::string ppb;
        probe.SerializeToString(&ppb);
        m_grpc->stream_send(sid, ppb);
      },
      [this](std::int32_t sid, const std::string &msg_pb) {
        tnmi::DeviceResponse resp;
        if (!resp.ParseFromString(msg_pb)) {
          tunnel_log("[mgmt] unparsable DeviceResponse (" +
                     std::to_string(msg_pb.size()) + " bytes)");
          return;
        }
        if (resp.fake())
          return; // heartbeat — carries no real data
        mgmt_hub::instance().note_device(m_grpc.get(), sid, resp.device_id());

        // Identity probe reply (from session-open): extract hostname/role, set
        // the session identity (drives the prompt), and print a banner — but do
        // NOT render it as a normal reply.
        if (resp.rpc_id() == "__mgmt_probe__") {
          std::string hostname, role;
          if (resp.response().Is<gnmi::GetResponse>()) {
            gnmi::GetResponse gr;
            resp.response().UnpackTo(&gr);
            auto unq = [](std::string v) {
              if (v.size() >= 2 && v.front() == '"' && v.back() == '"')
                v = v.substr(1, v.size() - 2);
              return v;
            };
            auto ends = [](const std::string &p, const char *suf) {
              const std::string s(suf);
              return p.size() >= s.size() &&
                     p.compare(p.size() - s.size(), s.size(), s) == 0;
            };
            for (const auto &n : gr.notification()) {
              const std::string prefix = gnmi_util::path_to_string(n.prefix());
              for (const auto &u : n.update()) {
                const std::string p = prefix + gnmi_util::path_to_string(u.path());
                if (ends(p, "hostname"))
                  hostname = unq(gnmi_util::typed_value_to_json(u.val()));
                else if (ends(p, "role"))
                  role = unq(gnmi_util::typed_value_to_json(u.val()));
              }
            }
          }
          mgmt_hub::instance().set_identity(m_grpc.get(), sid, hostname, role);

          std::string who;
          if (!role.empty() && !hostname.empty()) who = role + "(" + hostname + ")";
          else if (!hostname.empty()) who = hostname;
          else if (!role.empty()) who = role;
          else who = resp.device_id().empty() ? "device" : resp.device_id();
          tunnel_log("\xE2\x96\xB8 " + who + " connected" +
                     (resp.device_id().empty()
                          ? ""
                          : "  \xC2\xB7  device " + resp.device_id())); // ▸ ·
          return;
        }

        const std::string cmd = mgmt_hub::instance().command_for(resp.rpc_id());
        std::string hdr = "[mgmt] " + (cmd.empty() ? std::string("push")
                                                   : "reply '" + cmd + "'");
        if (!resp.rpc_id().empty()) hdr += "  rpc=" + resp.rpc_id();
        if (!resp.device_id().empty()) hdr += "  dev=" + resp.device_id();
        tunnel_log(hdr);

        // Unpack a CliResponse if that is what the Any carries.
        if (resp.response().Is<tnmi::DeviceResponse::CliResponse>()) {
          tnmi::DeviceResponse::CliResponse cli;
          resp.response().UnpackTo(&cli);
          tunnel_log("    exit=" + std::to_string(cli.exit_code()) +
                     (cli.timeout() ? " (timeout)" : "") +
                     (cli.truncated() ? " (truncated)" : "") + "  " +
                     std::to_string(cli.duration_ms()) + "ms");
          if (!cli.stdout_op().empty())
            tunnel_log(cli.stdout_op());
          if (!cli.stderr_op().empty())
            tunnel_log("[stderr] " + cli.stderr_op());
        } else if (resp.response().Is<gnmi::GetResponse>()) {
          gnmi::GetResponse gr;
          resp.response().UnpackTo(&gr);
          for (const auto &n : gr.notification()) {
            const std::string prefix = gnmi_util::path_to_string(n.prefix());
            for (const auto &u : n.update())
              tunnel_log("    " + prefix + gnmi_util::path_to_string(u.path()) +
                         " = " + gnmi_util::typed_value_to_json(u.val()));
          }
        } else if (resp.response().Is<gnmi::SubscribeResponse>()) {
          gnmi::SubscribeResponse sr;
          resp.response().UnpackTo(&sr);
          tunnel_log("    " + gnmi_util::subscribe_response_to_json(sr));
        } else if (resp.response().Is<gnmi::SetResponse>()) {
          gnmi::SetResponse sr;
          resp.response().UnpackTo(&sr);
          tunnel_log("    set OK, " + std::to_string(sr.response_size()) +
                     " result(s)");
        } else if (!resp.response().type_url().empty()) {
          tunnel_log("    response: " + resp.response().type_url());
        }
      });
}

// ---------------------------------------------------------------------------
// gNMI Subscribe (server-streaming)
// ---------------------------------------------------------------------------

void connected_client::start_subscription(std::int32_t stream_id,
                                          const std::string &request_pb) {
  gnmi::SubscribeRequest req;
  if (!req.ParseFromString(request_pb)) {
    m_grpc->stream_finish(stream_id, 3); // INVALID_ARGUMENT
    return;
  }

  const gnmi::SubscriptionList &sl = req.subscribe();
  std::vector<std::string> paths;
  for (const auto &s : sl.subscription())
    paths.push_back(gnmi_util::path_to_string(s.path()));
  const bool streaming = (sl.mode() == gnmi::SubscriptionList::STREAM);

  std::cout << "[Subscribe] stream=" << stream_id << " paths=" << paths.size()
            << " mode=" << (streaming ? "STREAM" : "ONCE/POLL") << "\n";

  // No datastore here, so there are no current values to dump — send
  // sync_response immediately to mark the end of the (empty) initial state.
  gnmi::SubscribeResponse sync;
  sync.set_sync_response(true);
  std::string out;
  sync.SerializeToString(&out);
  m_grpc->stream_send(stream_id, out);

  if (streaming) {
    // Register for on-change telemetry: real Set pushes (from this or any
    // other connection) are fanned out to this stream by sub_hub.
    sub_hub::instance().add(this, m_grpc.get(), stream_id, std::move(paths));
  } else {
    m_grpc->stream_finish(stream_id, 0); // ONCE: nothing to stream
  }
}

// ---------------------------------------------------------------------------
// Remaining virtual overrides
// ---------------------------------------------------------------------------

std::int32_t connected_client::handle_event(const std::int32_t & /*channel*/,
                                            const std::uint16_t & /*event*/) {
  return 0;
}

std::int32_t connected_client::handle_write(
    const std::int32_t & /*channel*/) {
  return 0;
}

std::int32_t connected_client::handle_close(const std::int32_t &channel) {
  // Drop any Subscribe streams this connection owns before it is destroyed, so
  // the hub never holds a dangling grpc_session pointer.
  sub_hub::instance().remove(this);
  // Drop any grpctunnel targets registered over this connection's Register
  // stream so the registry never holds a dangling grpc_session pointer.
  for (const auto &id : tunnel_hub::instance().remove_by_stream(m_grpc.get()))
    tunnel_log("[reg] -target '" + id + "' (disconnected)");
  // Drop any mgmt dial-out (DialTcc.Subscribe) session on this connection.
  for (const int id : mgmt_hub::instance().remove_by_stream(m_grpc.get()))
    tunnel_log("[mgmt] session #" + std::to_string(id) + " closed");
  // Tell the server to remove this connection from its client map.
  // server::handle_close erases the unique_ptr<connected_client>, destroying
  // this object — no member access is safe after this call returns.
  if (m_parent)
    m_parent->handle_close(channel);
  return 0;
}

#endif
