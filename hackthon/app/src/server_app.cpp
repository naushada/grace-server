#ifndef __server_app_cpp__
#define __server_app_cpp__

#include "server_app.hpp"
#include "client_app.hpp"
#include "framework.hpp"

std::int32_t server::handle_connect(const std::int32_t &channel,
                                    const std::string &peer_host) {
  // wrap_accepted() returns a TLS bufferevent when this server was constructed
  // with a TLS ctx; otherwise returns a plain socket bufferevent.
  auto *bev = wrap_accepted(channel);
  auto clnt = std::make_unique<connected_client>(bev, peer_host, this);
  auto res = clients().insert(std::make_pair(channel, std::move(clnt)));
  if (!res.second) {
    std::cout << "Fn:" << __func__ << ":" << __LINE__
              << " New client insersion for channel:" << channel << " failed"
              << std::endl;
  }
  return res.second ? 0 : -1;
}

std::int32_t server::handle_close(const std::int32_t &channel) {
  return (clients().erase(channel));
}

std::int32_t server::handle_accept(const std::int32_t &channel,
                                   const std::string &peer_host) {
  return (0);
}
#endif
