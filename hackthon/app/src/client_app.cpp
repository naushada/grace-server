#ifndef __client_app_cpp__
#define __client_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"
#include "server_app.hpp"

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

        std::cout << "[Get] path_count=" << req.path_size() << "\n";

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

        std::cout << "[Set] update_count=" << req.update_size()
                  << " replace_count=" << req.replace_size()
                  << " delete_count=" << req.delete__size() << "\n";

        gnmi::SetResponse resp;
        // Reflect back each operation as OK.
        for (int i = 0; i < req.update_size(); ++i) {
          auto *r = resp.add_response();
          *r->mutable_path() = req.update(i).path();
          r->set_op(gnmi::UpdateResult::UPDATE);
        }
        for (int i = 0; i < req.replace_size(); ++i) {
          auto *r = resp.add_response();
          *r->mutable_path() = req.replace(i).path();
          r->set_op(gnmi::UpdateResult::REPLACE);
        }
        for (int i = 0; i < req.delete__size(); ++i) {
          auto *r = resp.add_response();
          *r->mutable_path() = req.delete_(i);
          r->set_op(gnmi::UpdateResult::DELETE);
        }

        std::string out;
        resp.SerializeToString(&out);
        return {0, out};
      });

  // ----- Subscribe ----------------------------------------------------------
  // Subscribe is a bidirectional-streaming RPC which grpc_session does not
  // yet support natively.  Return UNIMPLEMENTED until streaming is wired in.
  m_grpc->register_unary(
      "/gnmi.gNMI/Subscribe",
      [](const std::string &) -> std::pair<int, std::string> {
        return {12, ""}; // 12 = UNIMPLEMENTED
      });
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
