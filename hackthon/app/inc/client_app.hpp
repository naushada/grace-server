#ifndef __client_app_hpp__
#define __client_app_hpp__

#include "framework.hpp"
#include "http2.hpp"

#include <memory>

class server;

class connected_client : public evt_io {
public:
  connected_client(const std::int32_t &channel, const std::string &peer_host,
                   server *parent)
      : evt_io(channel, peer_host), m_parent(parent),
        m_http2(std::make_unique<http2_session>(
            /*server_side=*/true,
            [this](int32_t stream_id, const http2_session::request &req) {
              on_http2_request(stream_id, req);
            })) {
    // Send the server's initial SETTINGS frame as soon as the connection opens.
    auto out = m_http2->drain_send_buf();
    if (!out.empty())
      tx(out.data(), out.size());
  }

  virtual ~connected_client() {
    std::cout << "Fn:" << __func__ << ":" << __LINE__ << " dtor" << std::endl;
  }

  server &parent() const { return *m_parent; }

  virtual std::int32_t handle_read(const std::int32_t &channel,
                                   const std::string &data,
                                   const bool &dry_run) override;
  virtual std::int32_t handle_event(const std::int32_t &channel,
                                    const std::uint16_t &event) override;
  virtual std::int32_t handle_write(const std::int32_t &channel) override;
  virtual std::int32_t handle_close(const std::int32_t &channel) override;

private:
  // Called by the HTTP/2 session when a complete request has been received.
  void on_http2_request(int32_t stream_id, const http2_session::request &req);

  server *m_parent;
  std::unique_ptr<http2_session> m_http2;
};

#endif // !__client_app_hpp__
