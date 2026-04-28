#ifndef __framework_cpp__
#define __framework_cpp__

#include "framework.hpp"
#include "fs_app.hpp"

extern "C" {
#include <netdb.h>
#include <sys/socket.h>
}

// Client related callback
//
// All three callbacks cast ctx to evt_io* so they work for any subclass
// (connected_client, gnmi_connection, etc.).  Virtual dispatch takes care
// of calling the right override.

void client_event_cb(struct bufferevent *bev, short events, void *ctx) {
  if (bev == nullptr)
    return;

  auto channel = bufferevent_getfd(bev);
  auto io = static_cast<evt_io *>(ctx);

  if (events & BEV_EVENT_CONNECTED) {
    // Outbound connection established — let the subclass start its protocol.
    io->handle_connect(channel, "");
  } else if ((events & BEV_EVENT_EOF) || (events & BEV_EVENT_ERROR)) {
    // Peer closed or network error.  handle_close() is responsible for any
    // parent-notification (e.g. connected_client tells server to erase it).
    io->handle_close(channel);
  } else if (events & BEV_EVENT_TIMEOUT) {
    io->handle_event(channel, static_cast<std::uint16_t>(events));
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
  auto io = static_cast<evt_io *>(ctx);

  if (!io->handle_read(channel, data, dry_run)) {
    io->handle_read(channel, data, !dry_run);
    evbuffer_drain(input, nbytes);
  }
}

void client_write_cb(struct bufferevent *bev, void *ctx) {
  auto io = static_cast<evt_io *>(ctx);
  auto channel = bufferevent_getfd(bev);
  io->handle_write(channel);
}

// Server related callback
void server_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *address, int socklen, void *ctx) {
  char host[NI_MAXHOST];
  auto res = getnameinfo(address, socklen, host, sizeof(host), nullptr, 0,
                         NI_NUMERICHOST);
  if (!res) {
    auto io = static_cast<evt_io *>(ctx);
    io->handle_connect(fd, host);
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
// ---------------------------------------------------------------------------
// Raw-fd callbacks — used by evt_io(fd, rawfd_tag{})
// ---------------------------------------------------------------------------

static void raw_read_cb(evutil_socket_t fd, short, void *ctx) {
  static_cast<evt_io *>(ctx)->handle_read(static_cast<std::int32_t>(fd), "", false);
}

static void raw_write_cb(evutil_socket_t fd, short, void *ctx) {
  static_cast<evt_io *>(ctx)->handle_write(static_cast<std::int32_t>(fd));
}

evt_io::evt_io(evutil_socket_t fd, rawfd_tag)
    : m_from_host("rawfd"), m_buffer_evt_p(nullptr), m_listener_p(nullptr) {
  if (fd < 0) return; // failed connection — no events to register
  evutil_make_socket_nonblocking(fd);
  m_raw_read_ev = event_new(evt_base::instance().get(), fd,
                             EV_READ | EV_PERSIST, raw_read_cb, this);
  m_raw_write_ev = event_new(evt_base::instance().get(), fd,
                              EV_WRITE | EV_PERSIST, raw_write_cb, this);
  event_add(m_raw_read_ev, nullptr);
  // Write event is armed on demand via raw_watch_write(true)
}

void evt_io::raw_watch_write(bool enable) {
  if (!m_raw_write_ev) return;
  if (enable) event_add(m_raw_write_ev, nullptr);
  else        event_del(m_raw_write_ev);
}

// ---------------------------------------------------------------------------
// Default evt_io virtual implementations
// ---------------------------------------------------------------------------

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
