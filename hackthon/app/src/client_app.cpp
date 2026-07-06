#ifndef __client_app_cpp__
#define __client_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"
#include "gnmi_util.hpp"
#include "server_app.hpp"
#include "sub_hub.hpp"
#include "tunnel_hub.hpp"
#include "update_sink.hpp"

// Generated protobuf headers (produced by protoc at build time under
// ${CMAKE_BINARY_DIR}/app/proto_gen/).
#include "gnmi/gnmi.pb.h"
#include "tunnel/tunnel.pb.h"

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <iostream>

// Process-wide: forward gNMI over the tunnel (set by --mode=grpc-tunnel-server).
bool connected_client::s_tunnel_forward = false;

// Forward one serialised gNMI request to a dial-out target over the tunnel and
// arrange for its reply to complete the operator's async response. `respond`
// self-guards against the operator connection closing first.
static void forward_gnmi_over_tunnel(const char *method,
                                     const std::string &target,
                                     const std::string &req_pb,
                                     grpc_session::respond_fn respond) {
  if (target.empty()) {
    respond(3, ""); // INVALID_ARGUMENT — prefix.target names the device
    return;
  }
  if (!tunnel_hub::instance().connected(target)) {
    std::cerr << "[tunnel] " << method << " for '" << target
              << "': target not connected\n";
    respond(14, ""); // UNAVAILABLE
    return;
  }
  const std::uint64_t id = tunnel_hub::instance().next_id();
  tunnel::TunnelRequest treq;
  treq.set_id(id);
  treq.set_method(method);
  treq.set_payload(req_pb);
  tunnel_hub::instance().add_pending(
      id, [respond](int status, const std::string &payload) {
        respond(status, payload);
      });
  if (!tunnel_hub::instance().send(target, treq)) {
    tunnel_hub::instance().cancel_pending(id);
    respond(14, "");
    return;
  }
  std::cout << "[tunnel] -> '" << target << "' " << method << " id=" << id
            << " (" << req_pb.size() << " req bytes)\n";
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

  // ----- Tarana DialTcc / IsAlive -------------------------------------------
  // Tarana radios/BNs dial out to their controller and probe liveness with a
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

  // ----- Tarana DialTcc / PushSubscriptionUpdates (client-streaming) --------
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

        // Emit one readable "path = value" per leaf (full path = prefix + path)
        // instead of a single minified-JSON blob. Far easier to read/scroll/grep.
        // A header line carries the notification's timestamp (the sample time,
        // shared by every leaf in this notification) and the update count.
        const gnmi::Notification &n = resp.update();
        const std::string prefix = gnmi_util::path_to_string(n.prefix());
        const std::string ts = format_ns_timestamp(n.timestamp());
        update_sink::instance().emit("── " + ts + " · " +
                                     std::to_string(n.update_size()) +
                                     " update(s) ──");
        std::cout << "[PushSub] stream=" << sid << " " << ts << " " << prefix
                  << " (" << n.update_size() << " updates, " << n.delete__size()
                  << " deletes)\n";
        for (const auto &u : n.update())
          update_sink::instance().emit(prefix +
                                       gnmi_util::path_to_string(u.path()) +
                                       " = " +
                                       gnmi_util::typed_value_to_json(u.val()));
        for (int i = 0; i < n.delete__size(); ++i)
          update_sink::instance().emit(
              prefix + gnmi_util::path_to_string(n.delete_(i)) + " = (deleted)");
      });

  // ----- Tunnel: dial-out control channel (server side, increment 1) --------
  // A target dials in and opens /tunnel.Tunnel/Session, then sends a Register
  // frame identifying itself. We record it in tunnel_hub so the server can push
  // TunnelRequest frames DOWN to that target (e.g. gNMI Get/Set/Subscribe —
  // wired in increment 2). Reply frames are logged for now.
  m_grpc->register_bidi_stream(
      "/tunnel.Tunnel/Session",
      [](std::int32_t sid) {
        std::cout << "[tunnel] session opened stream=" << sid << "\n";
      },
      [this](std::int32_t sid, const std::string &msg_pb) {
        tunnel::TunnelResponse resp;
        if (!resp.ParseFromString(msg_pb)) {
          std::cerr << "[tunnel] stream=" << sid << " unparsable response\n";
          return;
        }
        switch (resp.body_case()) {
        case tunnel::TunnelResponse::kRegister: {
          const std::string &tid = resp.register_().target_id();
          tunnel_hub::instance().add(tid, m_grpc.get(), sid);
          std::cout << "[tunnel] target '" << tid << "' registered (stream="
                    << sid << ", " << tunnel_hub::instance().size()
                    << " connected)\n";
          break;
        }
        case tunnel::TunnelResponse::kPayload:
          // Reply from the target — complete the waiting operator request.
          tunnel_hub::instance().complete(resp.id(), 0, resp.payload());
          break;
        case tunnel::TunnelResponse::kError:
          std::cerr << "[tunnel] target error id=" << resp.id() << ": "
                    << resp.error() << "\n";
          tunnel_hub::instance().complete(resp.id(), 2 /*UNKNOWN*/, "");
          break;
        default:
          break;
        }
      });

  // ----- gNMI forwarding over the tunnel (--mode=grpc-tunnel-server) ---------
  // When forwarding is on, Get/Set are answered by relaying to the dial-out
  // target named in prefix.target (async: the operator's response completes
  // when the target replies). Registered as async unary, which takes precedence
  // over the local stubs above for the same path.
  if (s_tunnel_forward) {
    m_grpc->register_unary_async(
        "/gnmi.gNMI/Get",
        [](std::int32_t /*sid*/, const std::string &req_pb,
           grpc_session::respond_fn respond) {
          gnmi::GetRequest req;
          if (!req.ParseFromString(req_pb)) {
            respond(3, "");
            return;
          }
          forward_gnmi_over_tunnel("/gnmi.gNMI/Get", req.prefix().target(),
                                   req_pb, std::move(respond));
        });
    m_grpc->register_unary_async(
        "/gnmi.gNMI/Set",
        [](std::int32_t /*sid*/, const std::string &req_pb,
           grpc_session::respond_fn respond) {
          gnmi::SetRequest req;
          if (!req.ParseFromString(req_pb)) {
            respond(3, "");
            return;
          }
          forward_gnmi_over_tunnel("/gnmi.gNMI/Set", req.prefix().target(),
                                   req_pb, std::move(respond));
        });
  }
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
  // Likewise drop any tunnel sessions registered by this connection.
  tunnel_hub::instance().remove(m_grpc.get());
  // Tell the server to remove this connection from its client map.
  // server::handle_close erases the unique_ptr<connected_client>, destroying
  // this object — no member access is safe after this call returns.
  if (m_parent)
    m_parent->handle_close(channel);
  return 0;
}

#endif
