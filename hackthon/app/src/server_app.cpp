#ifndef __server_app_cpp__
#define __server_app_cpp__

#include "server_app.hpp"
#include "client_app.hpp"
#include "framework.hpp"

std::int32_t server::handle_connect(const std::int32_t &channel,
                                    const std::string &peer_host) {

  auto clnt = std::make_unique<connected_client>(channel, peer_host, this);
  clients().emplace(channel, std::move(clnt));
  // m_clients.at(fd) = std::move(clnt);
}

std::int32_t server::handle_close(const std::int32_t &channel) { return (0); }

std::int32_t server::handle_accept(const std::int32_t &channel,
                                   const std::string &peer_host) {
  return (0);
}
#endif
