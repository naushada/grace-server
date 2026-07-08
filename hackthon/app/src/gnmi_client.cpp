// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#ifndef __gnmi_client_cpp__
#define __gnmi_client_cpp__

#include "gnmi_client.hpp"
#include "framework.hpp"    // evt_io, evt_base
#include "grpc_session.hpp" // encode_frame / decode_frame
#include "http2.hpp"
#include "tls_config.hpp"

#include <algorithm>
#include <arpa/inet.h> // ntohl
#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

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
                  const std::string &rpc_path, const std::string &request_pb,
                  const tls_config &tls,
                  gnmi_client::response_cb on_done = {})
      : evt_io(host, port, tls.build_client_ctx()),
        m_host(host), m_rpc_path(rpc_path), m_request_pb(request_pb),
        m_on_done(std::move(on_done)),
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
      finish({-1, "http2 protocol error", ""});
      return static_cast<std::int32_t>(consumed);
    }

    flush(); // drain SETTINGS ACK, WINDOW_UPDATE, etc.
    return static_cast<std::int32_t>(consumed);
  }

  // Called by client_event_cb on BEV_EVENT_TIMEOUT.
  std::int32_t handle_event(const std::int32_t & /*channel*/,
                             const std::uint16_t & /*events*/) override {
    std::cerr << "[gnmi_connection] timed out\n";
    finish({-1, "timeout", ""});
    return 0;
  }

  // Called by client_event_cb on BEV_EVENT_EOF / BEV_EVENT_ERROR.
  std::int32_t handle_close(const std::int32_t & /*channel*/) override {
    if (!m_done) {
      std::cerr << "[gnmi_connection] connection closed before response\n";
      finish({-1, "connection closed before response", ""});
    }
    return 0;
  }

  std::int32_t handle_write(const std::int32_t & /*channel*/) override {
    return 0;
  }

private:
  // Drain all pending HTTP/2 frames into the bufferevent send buffer.
  void flush() {
    auto out = m_h2.drain_send_buf();
    if (!out.empty())
      tx(out.data(), out.size());
  }

  // Set the response, mark done, and invoke the callback exactly once.
  void finish(gnmi_client::response r) {
    if (m_done) return;
    m_response = std::move(r);
    m_done = true;
    if (m_on_done) m_on_done(m_response);
  }

  // Fires from the http2_session handler when the trailing HEADERS frame
  // (grpc-status) arrives with END_STREAM.
  void capture_response(const http2_session::request &resp) {
    gnmi_client::response r;
    auto sit = resp.headers.find("grpc-status");
    if (sit != resp.headers.end())
      r.grpc_status = std::stoi(sit->second);

    auto mit = resp.headers.find("grpc-message");
    if (mit != resp.headers.end())
      r.grpc_message = mit->second;

    std::string body_copy = resp.body;
    r.body_pb = grpc_session::decode_frame(body_copy);
    finish(std::move(r));
  }

  std::string m_host;
  std::string m_rpc_path;
  std::string m_request_pb;
  gnmi_client::response_cb m_on_done;
  http2_session m_h2;
  gnmi_client::response m_response;
  bool m_done{false};
};

// ---------------------------------------------------------------------------
// gnmi_client::call
// ---------------------------------------------------------------------------

gnmi_client::response gnmi_client::call(const std::string &host, uint16_t port,
                                         const std::string &rpc_path,
                                         const std::string &request_pb,
                                         const tls_config &tls) {
  gnmi_connection conn(host, port, rpc_path, request_pb, tls);

  // Drive the shared libevent event loop one iteration at a time until the
  // gRPC response is complete (or a timeout / error fires).  Other events
  // already registered (inotify from fs_app, etc.) are processed normally.
  // MUST be called outside event_base_dispatch (phased approach only).
  while (!conn.done()) {
    event_base_loop(evt_base::instance().get(), EVLOOP_ONCE);
  }

  return conn.take_response();
}

// ---------------------------------------------------------------------------
// push_async — non-blocking variant safe to call from inside the event loop.
//
// gnmi_connection is heap-allocated and stored in s_active so its lifetime
// spans the whole exchange.  Completed connections (done()==true) are lazily
// freed on the next push_async call.  The running event_base_dispatch drives
// the new connection's events without any re-entrancy.
// ---------------------------------------------------------------------------

// Intentionally leaked (reference to a heap vector): these connections own
// libevent bufferevents, and the event base is itself a static singleton.
// Never destroying them avoids a static-destruction-order crash where a
// bufferevent is freed after the base at process exit.
static auto &s_active = *new std::vector<std::shared_ptr<gnmi_connection>>();

void gnmi_client::push_async(const std::string &host, uint16_t port,
                               const std::string &rpc_path,
                               const std::string &request_pb,
                               const tls_config &tls,
                               response_cb on_done) {
  // Lazy GC: drop finished connections before adding a new one.
  s_active.erase(
      std::remove_if(s_active.begin(), s_active.end(),
                     [](const auto &c) { return c->done(); }),
      s_active.end());

  s_active.push_back(std::make_shared<gnmi_connection>(
      host, port, rpc_path, request_pb, tls, std::move(on_done)));

  std::cout << "[gnmi_client] push_async → " << host << ":" << port
            << " rpc=" << rpc_path << " active=" << s_active.size() << '\n';
}

// ---------------------------------------------------------------------------
// gnmi_sub_connection — a streaming gNMI Subscribe client.
//
// Mirrors gnmi_connection, but decodes SubscribeResponse messages incrementally
// from each DATA chunk (via http2_session::set_data_handler) instead of waiting
// for the stream to close. on_notif fires per message; on_done fires once when
// the stream's trailing HEADERS (grpc-status) arrive or the connection drops.
// ---------------------------------------------------------------------------

class gnmi_sub_connection : public evt_io {
public:
  gnmi_sub_connection(const std::string &host, uint16_t port,
                      const std::string &request_pb, const tls_config &tls,
                      gnmi_client::notification_cb on_notif,
                      gnmi_client::response_cb on_done)
      : evt_io(host, port, tls.build_client_ctx()), m_host(host),
        m_request_pb(request_pb), m_on_notif(std::move(on_notif)),
        m_on_done(std::move(on_done)),
        m_h2(/*server_side=*/false,
             [this](int32_t, const http2_session::request &resp) {
               capture_response(resp);
             }) {
    m_h2.set_data_handler(
        [this](int32_t, const uint8_t *d, size_t n) { on_data(d, n); });
  }

  ~gnmi_sub_connection() override = default;
  bool done() const { return m_done; }

  std::int32_t handle_connect(const std::int32_t &,
                              const std::string &) override {
    flush();
    const std::string framed = grpc_session::encode_frame(m_request_pb);
    m_h2.submit_request(
        "POST", "/gnmi.gNMI/Subscribe", m_host, "http",
        {{"content-type", "application/grpc+proto"}, {"te", "trailers"}},
        framed);
    flush();
    // Now that the connection is established, disable the inherited 5s
    // bufferevent read/write timeout so an idle subscription is not torn down
    // between updates. (Doing this in the ctor, mid-connect, breaks the
    // connect; here it is safe.)
    if (auto *bev = get_bufferevt())
      bufferevent_set_timeouts(bev, nullptr, nullptr);
    return 0;
  }

  std::int32_t handle_read(const std::int32_t &, const std::string &data,
                           const bool &dry_run) override {
    if (dry_run)
      return 0;
    const ssize_t consumed = m_h2.recv(
        reinterpret_cast<const uint8_t *>(data.data()), data.size());
    if (consumed < 0) {
      finish({-1, "http2 protocol error", ""});
      return static_cast<std::int32_t>(consumed);
    }
    flush();
    return static_cast<std::int32_t>(consumed);
  }

  std::int32_t handle_event(const std::int32_t &,
                            const std::uint16_t &) override {
    // A subscription idles between updates; the inherited 5s bufferevent read
    // timeout is not an error here. Re-enable reads and keep the stream open
    // (an actual EOF/error arrives via handle_close instead).
    if (auto *bev = get_bufferevt())
      bufferevent_enable(bev, EV_READ | EV_WRITE);
    return 0;
  }

  std::int32_t handle_close(const std::int32_t &) override {
    finish({0, "stream closed", ""});
    return 0;
  }

  std::int32_t handle_write(const std::int32_t &) override { return 0; }

private:
  void flush() {
    auto out = m_h2.drain_send_buf();
    if (!out.empty())
      tx(out.data(), out.size());
  }

  // Decode as many complete gRPC frames as m_rxbuf holds and deliver each.
  void on_data(const uint8_t *d, size_t n) {
    m_rxbuf.append(reinterpret_cast<const char *>(d), n);
    for (;;) {
      if (m_rxbuf.size() < 5)
        break;
      uint32_t len_be = 0;
      std::memcpy(&len_be, m_rxbuf.data() + 1, 4);
      const uint32_t len = ntohl(len_be);
      if (m_rxbuf.size() < 5u + len)
        break;
      std::string payload = m_rxbuf.substr(5, len);
      m_rxbuf.erase(0, 5u + len);
      if (m_on_notif)
        m_on_notif(payload);
    }
  }

  void capture_response(const http2_session::request &resp) {
    gnmi_client::response r{0, "", ""};
    auto sit = resp.headers.find("grpc-status");
    if (sit != resp.headers.end())
      r.grpc_status = std::stoi(sit->second);
    auto mit = resp.headers.find("grpc-message");
    if (mit != resp.headers.end())
      r.grpc_message = mit->second;
    finish(std::move(r));
  }

  void finish(gnmi_client::response r) {
    if (m_done)
      return;
    m_done = true;
    if (m_on_done)
      m_on_done(r);
  }

  std::string m_host;
  std::string m_request_pb;
  std::string m_rxbuf;
  gnmi_client::notification_cb m_on_notif;
  gnmi_client::response_cb m_on_done;
  http2_session m_h2;
  bool m_done{false};
};

// Leaked for the same reason as s_active (see above).
static auto &s_active_sub =
    *new std::vector<std::shared_ptr<gnmi_sub_connection>>();

void gnmi_client::subscribe_async(const std::string &host, uint16_t port,
                                  const std::string &request_pb,
                                  const tls_config &tls,
                                  notification_cb on_notif,
                                  response_cb on_done) {
  s_active_sub.erase(
      std::remove_if(s_active_sub.begin(), s_active_sub.end(),
                     [](const auto &c) { return c->done(); }),
      s_active_sub.end());

  s_active_sub.push_back(std::make_shared<gnmi_sub_connection>(
      host, port, request_pb, tls, std::move(on_notif), std::move(on_done)));

  std::cout << "[gnmi_client] subscribe_async → " << host << ":" << port
            << " active=" << s_active_sub.size() << '\n';
}

#endif // __gnmi_client_cpp__
