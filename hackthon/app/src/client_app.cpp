#ifndef __client_app_cpp__
#define __client_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"
#include "gnmi_util.hpp"
#include "server_app.hpp"
#include "update_sink.hpp"

// Generated protobuf headers (produced by protoc at build time under
// ${CMAKE_BINARY_DIR}/app/proto_gen/).
#include "gnmi/gnmi.pb.h"

#include <chrono>
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
  m_sub_stream = stream_id;
  m_sub_seq = 0;
  m_sub_paths.clear();
  for (const auto &s : sl.subscription())
    m_sub_paths.push_back(gnmi_util::path_to_string(s.path()));
  m_sub_streaming = (sl.mode() == gnmi::SubscriptionList::STREAM);

  std::cout << "[Subscribe] stream=" << stream_id
            << " paths=" << m_sub_paths.size()
            << " mode=" << (m_sub_streaming ? "STREAM" : "ONCE/POLL") << "\n";

  // Initial sample, then sync_response to mark the end of the initial dump.
  send_sub_notification();
  gnmi::SubscribeResponse sync;
  sync.set_sync_response(true);
  std::string out;
  sync.SerializeToString(&out);
  m_grpc->stream_send(stream_id, out);

  if (m_sub_streaming) {
    const struct timeval tv{1, 0}; // sample every second
    arm_timer(/*timer_id=*/1, tv, /*repeat=*/true);
  } else {
    m_grpc->stream_finish(stream_id, 0); // ONCE: close after the first batch
    m_sub_stream = -1;
  }
}

void connected_client::send_sub_notification() {
  if (m_sub_stream < 0)
    return;

  gnmi::SubscribeResponse resp;
  auto *notif = resp.mutable_update();
  const auto now = std::chrono::system_clock::now().time_since_epoch();
  notif->set_timestamp(
      std::chrono::duration_cast<std::chrono::nanoseconds>(now).count());
  for (const auto &p : m_sub_paths) {
    auto *u = notif->add_update();
    *u->mutable_path() = gnmi_util::parse_yang_path(p);
    u->mutable_val()->set_int_val(m_sub_seq);
  }
  std::string out;
  resp.SerializeToString(&out);
  m_grpc->stream_send(m_sub_stream, out);
}

std::int32_t connected_client::handle_timeout(int timer_id) {
  if (timer_id == 1 && m_sub_stream >= 0) {
    ++m_sub_seq;
    send_sub_notification();
  }
  return 0;
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
  // Tell the server to remove this connection from its client map.
  // server::handle_close erases the unique_ptr<connected_client>, destroying
  // this object — no member access is safe after this call returns.
  if (m_parent)
    m_parent->handle_close(channel);
  return 0;
}

#endif
