#ifndef __client_app_hpp__
#define __client_app_hpp__

#include "framework.hpp"
#include "grpc_session.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class server;

class connected_client : public evt_io {
public:
  // Constructor wires the grpc_session tx callback to this socket's tx() and
  // immediately registers all supported gNMI RPC handlers.  The grpc_session
  // ctor flushes the initial HTTP/2 SETTINGS frame via the tx callback so the
  // peer receives it as soon as the TCP connection is accepted.
  // Plain TCP: framework passes accepted fd directly.
  connected_client(const std::int32_t &channel, const std::string &peer_host,
                   server *parent)
      : evt_io(channel, peer_host), m_parent(parent),
        m_grpc(std::make_unique<grpc_session>(
            [this](const char *d, size_t n) { tx(d, n); })) {
    register_gnmi_handlers();
  }

  // TLS (or pre-wrapped plain) bev: server::handle_connect calls
  // wrap_accepted(fd) which returns a TLS bev when TLS ctx is set.
  connected_client(struct bufferevent *bev, const std::string &peer_host,
                   server *parent)
      : evt_io(bev, peer_host), m_parent(parent),
        m_grpc(std::make_unique<grpc_session>(
            [this](const char *d, size_t n) { tx(d, n); })) {
    register_gnmi_handlers();
  }

  virtual ~connected_client() {
    std::cout << "Fn:" << __func__ << ":" << __LINE__ << " dtor" << std::endl;
  }

  server &parent() const { return *m_parent; }

  // When true, connections forward gNMI Get/Set to a dial-out target over the
  // tunnel (by prefix.target) instead of answering from the local stub. Set by
  // main_app for --mode=grpc-tunnel-server; process-wide.
  static bool s_tunnel_forward;

  virtual std::int32_t handle_read(const std::int32_t &channel,
                                   const std::string &data,
                                   const bool &dry_run) override;
  virtual std::int32_t handle_event(const std::int32_t &channel,
                                    const std::uint16_t &event) override;
  virtual std::int32_t handle_write(const std::int32_t &channel) override;
  // handle_close notifies the parent server to erase this connection.
  // After that call this object is destroyed — no member access must follow.
  virtual std::int32_t handle_close(const std::int32_t &channel) override;

private:
  // Register unary + streaming gNMI RPC handlers on m_grpc.
  void register_gnmi_handlers();

  // gNMI Subscribe (server-streaming): register the stream with the server-wide
  // sub_hub so real Set pushes are fanned out to it as on-change notifications.
  // Sends sync_response immediately; ONCE mode then closes the stream.
  void start_subscription(std::int32_t stream_id, const std::string &request_pb);

  server *m_parent;
  std::unique_ptr<grpc_session> m_grpc;
};

#endif // !__client_app_hpp__
