#ifndef __grpc_session_cpp__
#define __grpc_session_cpp__

#include "grpc_session.hpp"

#include <arpa/inet.h> // htonl / ntohl
#include <cstring>
#include <iostream>

// ---------------------------------------------------------------------------
// Wire-format helpers
// ---------------------------------------------------------------------------

std::string grpc_session::encode_frame(const std::string &payload) {
  // 5-byte prefix: compressed-flag (1 byte) + big-endian length (4 bytes)
  const uint32_t len = static_cast<uint32_t>(payload.size());
  const uint32_t len_be = htonl(len);
  std::string frame(5, '\0');
  frame[0] = 0; // not compressed
  std::memcpy(&frame[1], &len_be, 4);
  frame.append(payload);
  return frame;
}

std::string grpc_session::decode_frame(std::string &buf) {
  // Need at least the 5-byte header
  if (buf.size() < 5)
    return {};

  uint32_t len_be = 0;
  std::memcpy(&len_be, buf.data() + 1, 4);
  const uint32_t len = ntohl(len_be);

  if (buf.size() < 5u + len)
    return {}; // incomplete

  std::string payload = buf.substr(5, len);
  buf.erase(0, 5 + len);
  return payload;
}

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

grpc_session::grpc_session(raw_tx_t tx)
    : m_h2(/*server_side=*/true,
            [this](int32_t sid, const http2_session::request &req) {
              on_request(sid, req);
            }),
      m_tx(std::move(tx)) {
  // Send the server's initial SETTINGS frame.
  flush();
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void grpc_session::register_unary(const std::string &path,
                                   unary_handler_t handler) {
  m_handlers[path] = std::move(handler);
}

ssize_t grpc_session::recv(const uint8_t *data, size_t len) {
  const ssize_t consumed = m_h2.recv(data, len);
  flush();
  return consumed;
}

void grpc_session::flush() {
  auto out = m_h2.drain_send_buf();
  if (!out.empty() && m_tx)
    m_tx(out.data(), out.size());
}

bool grpc_session::want_read() const { return m_h2.want_read(); }
bool grpc_session::want_write() const { return m_h2.want_write(); }

// ---------------------------------------------------------------------------
// Internal request dispatch
// ---------------------------------------------------------------------------

void grpc_session::on_request(int32_t stream_id,
                               const http2_session::request &req) {
  // Validate content-type
  auto ct_it = req.headers.find("content-type");
  if (ct_it == req.headers.end() ||
      ct_it->second.find("application/grpc") == std::string::npos) {
    // Not a gRPC request — return HTTP 415
    m_h2.submit_response(stream_id, 415, {}, "");
    flush();
    return;
  }

  auto it = m_handlers.find(req.path);
  if (it == m_handlers.end()) {
    // Unknown method — gRPC status 12 = UNIMPLEMENTED
    send_unary_response(stream_id, 12, "");
    return;
  }

  // Decode the length-prefixed body
  std::string body_copy = req.body;
  const std::string request_pb = decode_frame(body_copy);

  // Invoke the handler
  auto [grpc_status, response_pb] = it->second(request_pb);
  send_unary_response(stream_id, grpc_status, response_pb);
}

void grpc_session::send_unary_response(int32_t stream_id, int grpc_status,
                                        const std::string &body_pb) {
  const std::string grpc_status_str = std::to_string(grpc_status);
  const std::string framed = encode_frame(body_pb);

  // Send HEADERS (:status 200, content-type) followed by DATA
  // then trailing HEADERS (grpc-status).
  m_h2.submit_response(
      stream_id, 200,
      {{"content-type", "application/grpc+proto"}},
      framed,
      /*with_trailers=*/true);

  flush();

  // Trailing HEADERS carrying grpc-status — this closes the stream.
  m_h2.submit_trailer(stream_id, {{"grpc-status", grpc_status_str}});
  flush();
}

#endif // __grpc_session_cpp__
