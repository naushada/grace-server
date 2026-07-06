#ifndef __grpc_session_cpp__
#define __grpc_session_cpp__

#include "grpc_session.hpp"

#include <arpa/inet.h> // htonl / ntohl
#include <cstring>
#include <iostream>
#include <zlib.h> // gzip inflate for compressed gRPC frames

// ---------------------------------------------------------------------------
// gzip / zlib inflate
// ---------------------------------------------------------------------------
// gRPC's per-message Compressed-Flag (byte 0 of the 5-byte frame header) marks
// a body compressed with the encoding from the grpc-encoding header (dial-out
// clients advertise gzip). Inflate transparently so RPC handlers always receive
// raw protobuf. Returns "" on failure. windowBits 15+32 auto-detects a gzip or
// zlib wrapper.
namespace {
std::string gzip_inflate(const std::string &in) {
  if (in.empty())
    return {};

  z_stream zs{};
  if (inflateInit2(&zs, 15 + 32) != Z_OK)
    return {};
  zs.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(in.data()));
  zs.avail_in = static_cast<uInt>(in.size());

  std::string out;
  char buf[16384];
  int rc;
  do {
    zs.next_out = reinterpret_cast<Bytef *>(buf);
    zs.avail_out = sizeof(buf);
    rc = inflate(&zs, Z_NO_FLUSH);
    if (rc != Z_OK && rc != Z_STREAM_END) { // Z_DATA_ERROR, Z_BUF_ERROR, …
      inflateEnd(&zs);
      return {};
    }
    out.append(buf, sizeof(buf) - zs.avail_out);
  } while (rc != Z_STREAM_END);
  inflateEnd(&zs);
  return out;
}
} // namespace

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

  const uint8_t compressed_flag = static_cast<uint8_t>(buf[0]);
  uint32_t len_be = 0;
  std::memcpy(&len_be, buf.data() + 1, 4);
  const uint32_t len = ntohl(len_be);

  if (buf.size() < 5u + len)
    return {}; // incomplete

  std::string payload = buf.substr(5, len);
  buf.erase(0, 5 + len);

  // Compressed-Flag set: the body is compressed (gzip). Inflate so callers get
  // raw protobuf. On failure, log and return "" — the handler then sees an
  // empty request rather than garbage.
  if (compressed_flag & 0x01) {
    std::string inflated = gzip_inflate(payload);
    if (inflated.empty() && !payload.empty()) {
      std::cerr << "[grpc] gzip inflate failed (" << payload.size()
                << " compressed bytes)\n";
      return {};
    }
    return inflated;
  }
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
  // Deliver client-streaming request messages as they arrive (before
  // END_STREAM), consuming them from the stream's buffer.
  m_h2.set_request_stream_handler(
      [this](int32_t sid, const std::string &path, std::string &body,
             bool /*end_stream*/) { on_request_stream(sid, path, body); });
  m_h2.set_request_headers_handler(
      [this](int32_t sid, const http2_session::request &req) {
        on_request_headers(sid, req);
      });
  // Send the server's initial SETTINGS frame.
  flush();
}

grpc_session::~grpc_session() {
  // Invalidate any outstanding async `respond` callbacks so a late reply after
  // this connection closes is a safe no-op rather than a use-after-free.
  if (m_alive)
    *m_alive = false;
}

// ---------------------------------------------------------------------------
// Public interface
// ---------------------------------------------------------------------------

void grpc_session::register_unary(const std::string &path,
                                   unary_handler_t handler) {
  m_handlers[path] = std::move(handler);
}

void grpc_session::register_server_stream(const std::string &path,
                                          stream_handler_t handler) {
  m_stream_handlers[path] = std::move(handler);
}

void grpc_session::register_client_stream(const std::string &path,
                                          client_stream_handler_t handler) {
  m_client_stream_handlers[path] = std::move(handler);
}

void grpc_session::register_unary_async(const std::string &path,
                                        unary_async_handler_t handler) {
  m_async_handlers[path] = std::move(handler);
}

void grpc_session::register_bidi_stream(const std::string &path,
                                        bidi_open_handler_t on_open,
                                        client_stream_handler_t on_message) {
  m_bidi_open_handlers[path] = std::move(on_open);
  // Incoming messages reuse the client-streaming decode/dispatch path.
  m_client_stream_handlers[path] = std::move(on_message);
}

// Open the response side of a bidi stream when the client opens the request
// stream (HEADERS received). The app can then push frames with stream_send().
void grpc_session::on_request_headers(int32_t stream_id,
                                      const http2_session::request &req) {
  auto it = m_bidi_open_handlers.find(req.path);
  if (it == m_bidi_open_handlers.end())
    return; // not a bidi method — normal (unary/server-stream) dispatch applies
  if (m_bidi_opened[stream_id])
    return; // already opened for this stream
  m_bidi_opened[stream_id] = true;
  m_h2.submit_response_stream(
      stream_id, 200, {{"content-type", "application/grpc+proto"}});
  flush();
  it->second(stream_id);
}

void grpc_session::on_request_stream(int32_t stream_id,
                                     const std::string &path,
                                     std::string &body) {
  auto it = m_client_stream_handlers.find(path);
  if (it == m_client_stream_handlers.end())
    return; // not a client-streaming method — leave for END_STREAM dispatch

  // Extract and dispatch every complete gRPC message currently buffered.
  // decode_frame() removes each consumed frame from `body` (and gzip-inflates
  // it), so the buffer stays bounded and any partial trailing frame is kept
  // until the rest arrives.
  while (body.size() >= 5) {
    const size_t before = body.size();
    std::string msg = decode_frame(body);
    if (body.size() == before)
      break; // incomplete frame — wait for more DATA
    it->second(stream_id, msg);
  }
}

void grpc_session::stream_send(int32_t stream_id,
                               const std::string &message_pb) {
  m_h2.push_stream_data(stream_id, encode_frame(message_pb));
  flush();
}

void grpc_session::stream_finish(int32_t stream_id, int grpc_status) {
  m_h2.finish_stream(stream_id); // flush pending DATA + EOF (NO_END_STREAM)
  flush();
  m_h2.submit_trailer(stream_id,
                      {{"grpc-status", std::to_string(grpc_status)}});
  flush();
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

  // Client-streaming methods: messages were already decoded and dispatched
  // incrementally by on_request_stream() as DATA arrived. Reaching here means
  // the client half-closed (END_STREAM); drain any final buffered messages,
  // then close the call OK rather than falling through to UNIMPLEMENTED.
  auto cit = m_client_stream_handlers.find(req.path);
  if (cit != m_client_stream_handlers.end()) {
    std::string leftover = req.body;
    on_request_stream(stream_id, req.path, leftover);
    if (m_bidi_open_handlers.count(req.path)) {
      // Bidi: the streaming response is already open — close it with a trailer.
      stream_finish(stream_id, 0);
      m_bidi_opened.erase(stream_id);
    } else {
      send_unary_response(stream_id, 0, ""); // pure client-streaming
    }
    return;
  }

  // Decode the length-prefixed request body once (shared by both paths).
  std::string body_copy = req.body;
  const std::string request_pb = decode_frame(body_copy);

  // Server-streaming methods (e.g. gNMI Subscribe): open the response, then
  // hand control to the application which streams messages over time.
  auto sit = m_stream_handlers.find(req.path);
  if (sit != m_stream_handlers.end()) {
    m_h2.submit_response_stream(
        stream_id, 200, {{"content-type", "application/grpc+proto"}});
    flush();
    sit->second(stream_id, request_pb);
    return;
  }

  // Async unary methods (e.g. gNMI forwarded over a tunnel): the handler sends
  // the response later via the `respond` callback, which is a no-op if this
  // connection has closed by then.
  auto ait = m_async_handlers.find(req.path);
  if (ait != m_async_handlers.end()) {
    auto alive = m_alive;
    respond_fn respond = [this, alive, stream_id](int status,
                                                  const std::string &body) {
      if (!alive || !*alive)
        return; // connection closed — drop the late reply
      send_unary_response(stream_id, status, body);
    };
    ait->second(stream_id, request_pb, std::move(respond));
    return;
  }

  auto it = m_handlers.find(req.path);
  if (it == m_handlers.end()) {
    // Unknown method — gRPC status 12 = UNIMPLEMENTED. Log the path so an
    // operator can see exactly which RPC the client wanted but we don't serve
    // (e.g. a dial-out telemetry method that still needs a handler).
    std::cerr << "[grpc] UNIMPLEMENTED " << req.path << " (" << request_pb.size()
              << " req bytes)\n";
    send_unary_response(stream_id, 12, "");
    return;
  }

  // Invoke the unary handler
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
