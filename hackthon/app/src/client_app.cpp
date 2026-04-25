#ifndef __client_app_cpp__
#define __client_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"

#include <iostream>

std::int32_t connected_client::handle_read(const std::int32_t & /*channel*/,
                                           const std::string &data,
                                           const bool &dry_run) {
  if (dry_run)
    return 0;

  // Feed raw bytes from the socket into the HTTP/2 session.
  const auto consumed = m_http2->recv(
      reinterpret_cast<const uint8_t *>(data.data()), data.size());

  if (consumed < 0) {
    std::cerr << "Fn:" << __func__ << ":" << __LINE__
              << " nghttp2 recv error: " << nghttp2_strerror(consumed)
              << std::endl;
    return static_cast<std::int32_t>(consumed);
  }

  // Drain any frames nghttp2 produced (SETTINGS ACK, WINDOW_UPDATE, etc.)
  auto out = m_http2->drain_send_buf();
  if (!out.empty())
    tx(out.data(), out.size());

  return static_cast<std::int32_t>(consumed);
}

void connected_client::on_http2_request(int32_t stream_id,
                                         const http2_session::request &req) {
  std::cout << "Fn:" << __func__ << ":" << __LINE__
            << " HTTP/2 request stream_id:" << stream_id
            << " method:" << req.method << " path:" << req.path << std::endl;

  // Default: echo 200 OK with an empty JSON body. Derived classes or a
  // registered handler can replace this with real application logic.
  m_http2->submit_response(stream_id, 200,
                            {{"content-type", "application/json"}}, "{}");

  auto out = m_http2->drain_send_buf();
  if (!out.empty())
    tx(out.data(), out.size());
}

std::int32_t connected_client::handle_event(const std::int32_t & /*channel*/,
                                            const std::uint16_t & /*event*/) {
  return 0;
}

std::int32_t connected_client::handle_write(
    const std::int32_t & /*channel*/) {
  return 0;
}

std::int32_t connected_client::handle_close(
    const std::int32_t & /*channel*/) {
  return 0;
}

#endif
