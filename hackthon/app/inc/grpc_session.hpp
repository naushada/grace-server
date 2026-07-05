#ifndef __grpc_session_hpp__
#define __grpc_session_hpp__

// grpc_session implements the gRPC wire protocol on top of http2_session.
//
// gRPC over HTTP/2 wire format:
//   - Request path  : /package.Service/Method
//   - Content-Type  : application/grpc+proto
//   - Message frame : 1-byte compressed-flag | 4-byte big-endian length | body
//   - Trailers      : trailing HEADERS frame carrying grpc-status (+ grpc-message)
//
// This class handles framing transparently so callers work with plain
// std::string protobuf payloads.

#include "http2.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

class grpc_session {
public:
  // A unary RPC handler: receives serialised protobuf bytes, returns
  // {grpc_status_code, serialised_response_bytes}.
  // grpc_status 0 = OK.
  using unary_handler_t =
      std::function<std::pair<int, std::string>(const std::string &request_pb)>;

  // Construct a server-side gRPC session wrapping an HTTP/2 layer.
  // raw_tx is called whenever bytes need to be written to the socket.
  using raw_tx_t = std::function<void(const char *data, size_t len)>;
  explicit grpc_session(raw_tx_t tx);

  // A server-streaming RPC handler: receives the request protobuf bytes and the
  // stream id, and initiates streaming. It returns immediately; the application
  // pushes response messages later via stream_send() and closes the stream with
  // stream_finish(). Used for gNMI Subscribe.
  using stream_handler_t =
      std::function<void(int32_t stream_id, const std::string &request_pb)>;

  // Register a unary RPC handler for path "/package.Service/Method".
  void register_unary(const std::string &path, unary_handler_t handler);

  // Register a server-streaming handler for path "/package.Service/Method".
  void register_server_stream(const std::string &path, stream_handler_t handler);

  // A client-streaming RPC handler: invoked once per request message as it
  // arrives on the stream (the client keeps the stream open and pushes many
  // messages). Used for methods like tnmi.DialTcc/PushSubscriptionUpdates.
  using client_stream_handler_t =
      std::function<void(int32_t stream_id, const std::string &message_pb)>;

  // Register a client-streaming handler for path "/package.Service/Method".
  void register_client_stream(const std::string &path,
                              client_stream_handler_t handler);

  // Send one framed message on an open streaming response.
  void stream_send(int32_t stream_id, const std::string &message_pb);

  // Close a streaming response with the given grpc-status trailer.
  void stream_finish(int32_t stream_id, int grpc_status);

  // Feed raw bytes from the network.  Returns bytes consumed or <0 on error.
  ssize_t recv(const uint8_t *data, size_t len);

  // Drain and transmit any pending output.  Call after recv() and after
  // submitting responses.
  void flush();

  bool want_read() const;
  bool want_write() const;

  // Encode a single gRPC message frame (compressed=0 | 4-byte len | payload).
  static std::string encode_frame(const std::string &payload);

  // Decode the first gRPC message frame from buf.  Returns the payload and
  // removes it (plus the 5-byte header) from buf.  Returns "" if incomplete.
  static std::string decode_frame(std::string &buf);

private:
  void on_request(int32_t stream_id, const http2_session::request &req);
  // Decode and dispatch complete gRPC messages buffered on a client-streaming
  // request, consuming them from `body` so it stays bounded.
  void on_request_stream(int32_t stream_id, const std::string &path,
                         std::string &body);
  void send_unary_response(int32_t stream_id, int grpc_status,
                            const std::string &body_pb);

  http2_session m_h2;
  raw_tx_t m_tx;
  std::unordered_map<std::string, unary_handler_t> m_handlers;
  std::unordered_map<std::string, stream_handler_t> m_stream_handlers;
  std::unordered_map<std::string, client_stream_handler_t>
      m_client_stream_handlers;
};

#endif // __grpc_session_hpp__
