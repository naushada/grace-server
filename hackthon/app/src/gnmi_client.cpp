#ifndef __gnmi_client_cpp__
#define __gnmi_client_cpp__

#include "gnmi_client.hpp"
#include "framework.hpp"    // evt_io, evt_base
#include "grpc_session.hpp" // encode_frame / decode_frame
#include "http2.hpp"

#include <iostream>

// ---------------------------------------------------------------------------
// gnmi_connection
//
// Mirrors connected_client: inherits evt_io and overrides the same hook
// methods.  The difference is this side initiates the TCP connection
// (outbound) and drives a client-side HTTP/2 + gRPC exchange rather than
// serving an incoming request.
//
// Hook flow (mirrors client_app.cpp):
//
//   libevent fires client_event_cb (BEV_EVENT_CONNECTED)
//     → evt_io::handle_connect  ← gnmi_connection::handle_connect
//         flush HTTP/2 preface + SETTINGS
//         submit_request (gRPC POST)
//         flush request frames
//
//   libevent fires client_read_cb
//     → evt_io::handle_read     ← gnmi_connection::handle_read
//         m_h2.recv (HTTP/2 decode)
//         flush (SETTINGS ACK, WINDOW_UPDATE …)
//         http2_session handler fires on complete response → m_done = true
//
//   libevent fires client_event_cb (BEV_EVENT_EOF / BEV_EVENT_TIMEOUT)
//     → evt_io::handle_close / handle_event ← gnmi_connection overrides
//         set m_done = true with error response
// ---------------------------------------------------------------------------

class gnmi_connection : public evt_io {
public:
  gnmi_connection(const std::string &host, uint16_t port,
                  const std::string &rpc_path, const std::string &request_pb)
      : evt_io(host, port, /*outbound=*/true),
        m_host(host), m_rpc_path(rpc_path), m_request_pb(request_pb),
        m_h2(/*server_side=*/false,
             [this](int32_t, const http2_session::request &resp) {
               capture_response(resp);
             }) {}

  virtual ~gnmi_connection() = default;

  bool done() const { return m_done; }
  gnmi_client::response take_response() { return std::move(m_response); }

  // -------------------------------------------------------------------------
  // evt_io hook overrides — same signature as connected_client
  // -------------------------------------------------------------------------

  // Called by client_event_cb when the outbound TCP connection is established.
  std::int32_t handle_connect(const std::int32_t & /*channel*/,
                               const std::string & /*peer*/) override {
    // Send HTTP/2 client connection preface + SETTINGS.
    flush();
    // Wrap the serialised proto in the 5-byte gRPC length-prefix and POST it.
    const std::string framed = grpc_session::encode_frame(m_request_pb);
    m_h2.submit_request(
        "POST", m_rpc_path, m_host, "http",
        {{"content-type", "application/grpc+proto"}, {"te", "trailers"}},
        framed);
    flush();
    return 0;
  }

  // Called by client_read_cb when bytes arrive from the peer.
  // dry_run=true: always return 0 (can handle). dry_run=false: process.
  std::int32_t handle_read(const std::int32_t & /*channel*/,
                            const std::string &data,
                            const bool &dry_run) override {
    if (dry_run)
      return 0;

    const ssize_t consumed = m_h2.recv(
        reinterpret_cast<const uint8_t *>(data.data()), data.size());

    if (consumed < 0) {
      std::cerr << "[gnmi_connection] http2 recv error: " << consumed << "\n";
      m_response = {-1, "http2 protocol error", ""};
      m_done = true;
      return static_cast<std::int32_t>(consumed);
    }

    flush(); // drain SETTINGS ACK, WINDOW_UPDATE, etc.
    return static_cast<std::int32_t>(consumed);
  }

  // Called by client_event_cb on BEV_EVENT_TIMEOUT.
  std::int32_t handle_event(const std::int32_t & /*channel*/,
                             const std::uint16_t & /*events*/) override {
    std::cerr << "[gnmi_connection] timed out\n";
    m_response = {-1, "timeout", ""};
    m_done = true;
    return 0;
  }

  // Called by client_event_cb on BEV_EVENT_EOF / BEV_EVENT_ERROR.
  // Unlike connected_client there is no parent server to notify — just
  // mark the exchange as failed if we have not yet received the response.
  std::int32_t handle_close(const std::int32_t & /*channel*/) override {
    if (!m_done) {
      std::cerr << "[gnmi_connection] connection closed before response\n";
      m_response = {-1, "connection closed before response", ""};
      m_done = true;
    }
    return 0;
  }

  std::int32_t handle_write(const std::int32_t & /*channel*/) override {
    return 0;
  }

private:
  // Drain all pending HTTP/2 frames into the bufferevent send buffer.
  // Mirrors connected_client calling tx() after m_grpc operations.
  void flush() {
    auto out = m_h2.drain_send_buf();
    if (!out.empty())
      tx(out.data(), out.size());
  }

  // Fires from the http2_session handler when the trailing HEADERS frame
  // (grpc-status) arrives with END_STREAM.
  void capture_response(const http2_session::request &resp) {
    auto sit = resp.headers.find("grpc-status");
    if (sit != resp.headers.end())
      m_response.grpc_status = std::stoi(sit->second);

    auto mit = resp.headers.find("grpc-message");
    if (mit != resp.headers.end())
      m_response.grpc_message = mit->second;

    // Strip the 5-byte gRPC length-prefix from the body.
    std::string body_copy = resp.body;
    m_response.body_pb = grpc_session::decode_frame(body_copy);
    m_done = true;
  }

  std::string m_host;
  std::string m_rpc_path;
  std::string m_request_pb;
  http2_session m_h2;
  gnmi_client::response m_response;
  bool m_done{false};
};

// ---------------------------------------------------------------------------
// gnmi_client::call
// ---------------------------------------------------------------------------

gnmi_client::response gnmi_client::call(const std::string &host, uint16_t port,
                                         const std::string &rpc_path,
                                         const std::string &request_pb) {
  gnmi_connection conn(host, port, rpc_path, request_pb);

  // Drive the shared libevent event loop one iteration at a time until the
  // gRPC response is complete (or a timeout / error fires).  Other events
  // already registered (inotify from fs_app, etc.) are processed normally.
  while (!conn.done()) {
    event_base_loop(evt_base::instance().get(), EVLOOP_ONCE);
  }

  return conn.take_response();
}

#endif // __gnmi_client_cpp__
