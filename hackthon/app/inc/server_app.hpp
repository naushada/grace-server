#ifndef __server_app_hpp__
#define __server_app_hpp__

#include "framework.hpp"

class connected_client;

class server : public evt_io {
public:
  using handle_t = std::int32_t;
  using client_t =
      std::unordered_map<handle_t, std::unique_ptr<connected_client>>;

  server(const std::string &host, const std::uint16_t &port)
      : evt_io(host, port), m_clients() {}

  virtual ~server() { m_clients.clear(); }

  client_t &clients() { return m_clients; }

  std::int32_t handle_connect(const handle_t &channel,
                              const std::string &peer_host) override;

  std::int32_t handle_close(const handle_t &channel) override;

  std::int32_t handle_accept(const handle_t &channel,
                             const std::string &peer_host) override;

private:
  client_t m_clients;
};
#endif // !__server_app_hpp__
