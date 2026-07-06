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
#include <memory>
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
  ~grpc_session();

  // A server-streaming RPC handler: receives the request protobuf bytes and the
  // stream id, and initiates streaming. It returns immediately; the application
  // pushes response messages later via stream_send() and closes the stream with
  // stream_finish(). Used for gNMI Subscribe.
  using stream_handler_t =
      std::function<void(int32_t stream_id, const std::string &request_pb)>;

  // Register a unary RPC handler for path "/package.Service/Method".
  void register_unary(const std::string &path, unary_handler_t handler);

  // An async unary RPC handler: like unary, but the response is produced later
  // (possibly after an off-box round-trip). The handler is given a `respond`
  // callback to invoke — once — when the {status, response} is ready. Used to
  // forward gNMI over a tunnel and await the target's reply without blocking the
  // event loop. `respond` self-guards: it is a no-op if this connection has
  // since closed.
  using respond_fn =
      std::function<void(int status, const std::string &response_pb)>;
  using unary_async_handler_t = std::function<void(
      int32_t stream_id, const std::string &request_pb, respond_fn respond)>;
  void register_unary_async(const std::string &path,
                            unary_async_handler_t handler);

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

  // A bidirectional-streaming RPC: `on_open` fires when the client opens the
  // stream (the response is opened so the app can push with stream_send();
  // close with stream_finish()); `on_message` fires once per request message
  // the client sends. Used for a dial-out tunnel where the target holds the
  // stream open and the server pushes requests down it.
  using bidi_open_handler_t = std::function<void(int32_t stream_id)>;
  void register_bidi_stream(const std::string &path,
                            bidi_open_handler_t on_open,
                            client_stream_handler_t on_message);

  // A token that is true while this connection is alive and flips false when it
  // closes. Streaming relays (e.g. Subscribe over a tunnel) capture a copy to
  // guard cross-connection stream_send/stream_finish against use-after-free.
  std::shared_ptr<const bool> alive_token() const { return m_alive; }

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
  // (or bidi) request, consuming them from `body` so it stays bounded.
  void on_request_stream(int32_t stream_id, const std::string &path,
                         std::string &body);
  // Open a bidi response when the client opens a bidi-registered stream.
  void on_request_headers(int32_t stream_id,
                          const http2_session::request &req);
  void send_unary_response(int32_t stream_id, int grpc_status,
                            const std::string &body_pb);

  http2_session m_h2;
  raw_tx_t m_tx;
  std::unordered_map<std::string, unary_handler_t> m_handlers;
  std::unordered_map<std::string, unary_async_handler_t> m_async_handlers;
  std::unordered_map<std::string, stream_handler_t> m_stream_handlers;
  std::unordered_map<std::string, client_stream_handler_t>
      m_client_stream_handlers;
  std::unordered_map<std::string, bidi_open_handler_t> m_bidi_open_handlers;
  std::unordered_map<int32_t, bool> m_bidi_opened; // streams already opened
  // True while this connection is alive; async `respond` callbacks capture a
  // copy and no-op after the connection closes (see register_unary_async).
  std::shared_ptr<bool> m_alive{std::make_shared<bool>(true)};
};

#endif // __grpc_session_hpp__
