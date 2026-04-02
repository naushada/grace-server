#ifndef __framework_cpp__
#define __framework_cpp__

#include "framework.hpp"
#include "client_app.hpp"
#include "fs_app.hpp"
#include "server_app.hpp"

extern "C" {
#include <netdb.h>
#include <sys/socket.h>
}

// Client related callback
void client_event_cb(struct bufferevent *bev, short events, void *ctx) {
  if (bev == nullptr) {
    return;
  }
  auto channel = bufferevent_getfd(bev);
  auto clnt = static_cast<connected_client *>(ctx);

  if (events & BEV_EVENT_EOF) {
    clnt->handle_close(channel);
    clnt->parent().handle_close(channel);
  }

  if (events & BEV_EVENT_ERROR) {
    // Add error handling
    clnt->handle_close(channel);
    clnt->parent().handle_close(channel);
  }
}

void client_read_cb(struct bufferevent *bev, void *ctx) {
  if (bev == nullptr)
    return;

  auto channel = bufferevent_getfd(bev);
  struct evbuffer *input = bufferevent_get_input(bev);
  auto nbytes = evbuffer_get_length(input);

  std::string data((char *)evbuffer_pullup(input, nbytes), nbytes);

  auto dry_run = true;
  auto clnt = static_cast<connected_client *>(ctx);

  if (!clnt->handle_read(channel, data, dry_run)) {
    clnt->handle_read(channel, data, !dry_run);
    evbuffer_drain(input, nbytes);
  }
}

void client_write_cb(struct bufferevent *bev, void *ctx) {

  auto clnt = static_cast<connected_client *>(ctx);
  // tx buffer has free space in it, proceed next write
  auto channel = bufferevent_getfd(bev);
  clnt->handle_write(channel);
}

// Server related callback
void server_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *address, int socklen, void *ctx) {
  char host[NI_MAXHOST];
  auto res = getnameinfo(address, socklen, host, sizeof(host), nullptr, 0,
                         NI_NUMERICHOST);
  if (!res) {
    auto srv = static_cast<server *>(ctx);
    srv->handle_connect(fd, host);
  }
}

void fs_read_cb(struct bufferevent *bev, void *ctx) {
  if (bev == nullptr)
    return;

  std::cout << "Fn:" << __func__ << ":" << __LINE__ << " Entry" << std::endl;
  auto channel = bufferevent_getfd(bev);
  struct evbuffer *input = bufferevent_get_input(bev);
  auto nbytes = evbuffer_get_length(input);

  std::string data((char *)evbuffer_pullup(input, nbytes), nbytes);

  auto dry_run = true;
  auto clnt = static_cast<fs_app *>(ctx);

  if (!clnt->handle_read(channel, data, dry_run)) {
    clnt->handle_read(channel, data, !dry_run);
    evbuffer_drain(input, nbytes);
  }
}
std::int32_t evt_io::handle_read(const std::int32_t &channel,
                                 const std::string &data, const bool &dry_run) {
  return (0);
}

std::int32_t evt_io::handle_event(const std::int32_t &channel,
                                  const std::uint16_t &event) {
  return (0);
}

std::int32_t evt_io::handle_write(const std::int32_t &channel) { return (0); }

std::int32_t evt_io::handle_connect(const std::int32_t &channel,
                                    const std::string &peer_host) {
  return (0);
}

std::int32_t evt_io::handle_close(const std::int32_t &channel) { return (0); }

std::int32_t evt_io::handle_accept(const std::int32_t &channel,
                                   const std::string &peer_host) {
  return (0);
}

#endif
