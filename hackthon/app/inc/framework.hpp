#ifndef __framework_hpp__
#define __framework_hpp__

#include <iostream>
#include <memory>
#include <unordered_map>

extern "C" {
#include <arpa/inet.h>
#include <cstddef>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>
}

extern "C" {
// Client related callback
void client_event_cb(struct bufferevent *bev, short events, void *ctx);
void client_read_cb(struct bufferevent *bev, void *ctx);
void client_write_cb(struct bufferevent *bev, void *ctx);

// Server related callback
void server_accept_cb(struct evconnlistener *listener, evutil_socket_t fd,
                      struct sockaddr *address, int socklen, void *ctx);
}

class evt_base {

public:
  evt_base(std::int32_t priority = 0) : m_event_base_p(event_base_new()) {
    if (priority > 0 && priority <= 255) {
      event_base_priority_init(m_event_base_p.get(), priority);
    }
  }

  static evt_base &instance() {
    static evt_base evtbase;
    return evtbase;
  }

  evt_base(const evt_base &) = delete;
  evt_base &operator=(const evt_base &) = delete;

  struct event_base *operator->() const { return m_event_base_p.get(); }

  struct event_base &operator*() const { return *m_event_base_p; }

  struct event_base *get() const { return m_event_base_p.get(); }

  struct event_base *get() { return m_event_base_p.get(); }

  explicit operator bool() const { return (m_event_base_p != nullptr); }

private:
  struct custom_deleter {
    void operator()(struct event_base *evt) { event_base_free(evt); }
  };
  std::unique_ptr<struct event_base, custom_deleter> m_event_base_p;
};

class evt_io {
public:
  evt_io(const evutil_socket_t &channel, const std::string &peer_host)
      : m_from_host(peer_host),
        m_buffer_evt_p(bufferevent_socket_new(evt_base::instance().get(),
                                              channel, BEV_OPT_CLOSE_ON_FREE)),
        m_listener_p(nullptr) {
    evutil_make_socket_nonblocking(channel);
    bufferevent_setcb(m_buffer_evt_p.get(), client_read_cb, client_write_cb,
                      client_event_cb, this);

    auto events = EV_READ | EV_WRITE | EV_PERSIST;
    // Enable the events
    bufferevent_enable(m_buffer_evt_p.get(), events);
  }

  evt_io(const evutil_socket_t &channel) : evt_io(channel, std::string()) {}

  evt_io(const std::string &host, const std::uint16_t &port)
      : m_from_host(host), m_buffer_evt_p(nullptr), m_listener_p(nullptr) {

    struct addrinfo *result;
    struct sockaddr_in self_addr;
    auto s = getaddrinfo(host.data(), std::to_string(port).c_str(), nullptr,
                         &result);
    if (!s) {
      self_addr = *((struct sockaddr_in *)(result->ai_addr));
      freeaddrinfo(result);
    }

    // TCP server listener
    m_listener_p.reset(evconnlistener_new_bind(
        evt_base::instance().get(), server_accept_cb, this /*This is for *ctx*/,
        (LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_EXEC),
        32 /*backlog*/, (struct sockaddr *)&self_addr, sizeof(self_addr)));
  }

  std::int32_t tx(const char *buffer, const size_t &len) {
    return bufferevent_write(m_buffer_evt_p.get(), (void *)buffer, len);
  }

  virtual std::int32_t handle_read(const std::int32_t &channel,
                                   const std::string &data,
                                   const bool &dry_run);
  virtual std::int32_t handle_event(const std::int32_t &channel,
                                    const std::uint16_t &event);
  virtual std::int32_t handle_write(const std::int32_t &channel);
  virtual std::int32_t handle_connect(const std::int32_t &channel,
                                      const std::string &peer_host);
  virtual std::int32_t handle_close(const std::int32_t &channel);
  virtual std::int32_t handle_accept(const std::int32_t &channel,
                                     const std::string &peer_host);

  ~evt_io() {
    if (m_buffer_evt_p)
      m_buffer_evt_p.reset(nullptr);
    if (m_listener_p)
      m_listener_p.reset(nullptr);

    std::cout << "Fn:" << __func__ << ":" << __LINE__ << " dtor" << std::endl;
  }

  struct bufferevent *get_bufferevt() const { return m_buffer_evt_p.get(); }

private:
  struct custom_deleter {
    void operator()(struct bufferevent *bevt) { bufferevent_free(bevt); }
  };

  struct custom_deleter_listener {
    void operator()(struct evconnlistener *listener) {
      evconnlistener_free(listener);
    }
  };

  std::string m_from_host;
  std::unique_ptr<struct bufferevent, custom_deleter> m_buffer_evt_p;
  std::unique_ptr<struct evconnlistener, custom_deleter_listener> m_listener_p;
};

struct run_evt_loop {

  int operator()() {
    event_base_dispatch(evt_base::instance().get());
    return (0);
  }
};
#endif /* __framework_hpp__ */
