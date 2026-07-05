#ifndef __client_app_cpp__
#define __client_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"
#include "gnmi_util.hpp"
#include "server_app.hpp"
#include "sub_hub.hpp"
#include "update_sink.hpp"

// Generated protobuf headers (produced by protoc at build time under
// ${CMAKE_BINARY_DIR}/app/proto_gen/).
#include "gnmi/gnmi.pb.h"

#include <iostream>

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
        const gnmi::Notification &n = resp.update();
        const std::string prefix = gnmi_util::path_to_string(n.prefix());
        std::cout << "[PushSub] stream=" << sid << " " << prefix << " ("
                  << n.update_size() << " updates, " << n.delete__size()
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
  // Tell the server to remove this connection from its client map.
  // server::handle_close erases the unique_ptr<connected_client>, destroying
  // this object — no member access is safe after this call returns.
  if (m_parent)
    m_parent->handle_close(channel);
  return 0;
}

#endif
