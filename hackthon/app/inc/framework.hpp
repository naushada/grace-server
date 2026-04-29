#ifndef __framework_hpp__
#define __framework_hpp__

#include <iostream>
#include <map>
#include <memory>

extern "C" {
#include <arpa/inet.h>
#include <cstddef>
#include <netdb.h>
#include <event2/buffer.h>
#include <event2/bufferevent.h>
#include <event2/bufferevent_ssl.h>
#include <event2/event.h>
#include <event2/listener.h>
#include <event2/util.h>
}

#include <openssl/ssl.h>

// RAII wrapper for SSL_CTX — shared by the framework (evt_io TLS support) and
// tls_config (which builds the context from cert/key/ca paths).
struct ssl_ctx_deleter {
  void operator()(SSL_CTX *ctx) const noexcept { SSL_CTX_free(ctx); }
};
using ssl_ctx_ptr = std::unique_ptr<SSL_CTX, ssl_ctx_deleter>;

// RAII wrapper for a libevent struct event*.  Cancels and frees on destruction.
struct event_deleter {
  void operator()(struct event *e) const noexcept {
    if (e) { event_del(e); event_free(e); }
  }
};
using evt_timer = std::unique_ptr<struct event, event_deleter>;

// Tag type that selects the server-listener evt_io constructor overload,
// disambiguating it from the outbound-client constructor of the same arity.
struct listener_tag {};

// Tag for the raw-fd evt_io constructor — used by mqtt_io and similar classes
// that manage their own socket I/O via a third-party library (no bufferevent).
struct rawfd_tag {};

extern "C" {
// Client related callback
void client_event_cb(struct bufferevent *bev, short events, void *ctx);
void client_read_cb(struct bufferevent *bev, void *ctx);
void fs_read_cb(struct bufferevent *bev, void *ctx);
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

  evt_io(const evutil_socket_t &channel)
      : m_from_host(),
        m_buffer_evt_p(bufferevent_socket_new(evt_base::instance().get(),
                                              channel, BEV_OPT_CLOSE_ON_FREE)),
        m_listener_p(nullptr) {

    evutil_make_socket_nonblocking(channel);
    bufferevent_setcb(m_buffer_evt_p.get(), fs_read_cb, nullptr, nullptr, this);

    auto events = EV_READ | EV_WRITE | EV_PERSIST;
    // Enable the events
    bufferevent_enable(m_buffer_evt_p.get(), events);
  }

  // Outbound plain TCP client connection.
  evt_io(const std::string &host, uint16_t port, bool /*outbound*/)
      : m_from_host(host),
        m_buffer_evt_p(bufferevent_socket_new(evt_base::instance().get(),
                                              /*fd=*/-1,
                                              BEV_OPT_CLOSE_ON_FREE)),
        m_listener_p(nullptr) {
    bufferevent_setcb(m_buffer_evt_p.get(), client_read_cb, client_write_cb,
                      client_event_cb, this);
    bufferevent_enable(m_buffer_evt_p.get(), EV_READ | EV_WRITE | EV_PERSIST);
    const struct timeval tv{5, 0};
    bufferevent_set_timeouts(m_buffer_evt_p.get(), &tv, &tv);
    bufferevent_socket_connect_hostname(m_buffer_evt_p.get(), /*evdns=*/nullptr,
                                        AF_UNSPEC, host.c_str(),
                                        static_cast<int>(port));
  }

  // Outbound TLS (or plain when ctx is null) client connection.
  // Callers pass tls_config::build_client_ctx() which returns nullptr when TLS
  // is disabled — this constructor degrades gracefully to plain TCP in that case.
  evt_io(const std::string &host, uint16_t port, ssl_ctx_ptr ctx)
      : m_from_host(host),
        m_buffer_evt_p(nullptr),
        m_listener_p(nullptr),
        m_ssl_ctx(std::move(ctx)) {
    struct bufferevent *bev;
    if (m_ssl_ctx) {
      SSL *ssl = SSL_new(m_ssl_ctx.get());
      bev = bufferevent_openssl_socket_new(evt_base::instance().get(), -1, ssl,
                                            BUFFEREVENT_SSL_CONNECTING,
                                            BEV_OPT_CLOSE_ON_FREE);
    } else {
      bev = bufferevent_socket_new(evt_base::instance().get(), -1,
                                    BEV_OPT_CLOSE_ON_FREE);
    }
    m_buffer_evt_p.reset(bev);
    const struct timeval tv{5, 0};
    bufferevent_set_timeouts(bev, &tv, &tv);
    bufferevent_setcb(bev, client_read_cb, client_write_cb, client_event_cb, this);
    bufferevent_enable(bev, EV_READ | EV_WRITE | EV_PERSIST);
    bufferevent_socket_connect_hostname(bev, nullptr, AF_UNSPEC, host.c_str(),
                                         static_cast<int>(port));
  }

  // Plain TCP server listener.
  evt_io(const std::string &host, const std::uint16_t &port)
      : m_from_host(host), m_buffer_evt_p(nullptr), m_listener_p(nullptr) {
    init_listener(host, port);
  }

  // TLS server listener. listener_tag disambiguates from the outbound TLS
  // client constructor which has the same (host, port, ssl_ctx_ptr) signature.
  evt_io(const std::string &host, const std::uint16_t &port,
         ssl_ctx_ptr ssl_ctx, listener_tag)
      : m_from_host(host), m_buffer_evt_p(nullptr), m_listener_p(nullptr),
        m_ssl_ctx(std::move(ssl_ctx)) {
    init_listener(host, port);
  }

  // Raw-fd constructor: wraps an existing fd with raw libevent events instead
  // of a bufferevent.  Used by mqtt_io to integrate library-managed sockets.
  // EV_READ|EV_PERSIST is armed immediately; write events are disabled until
  // raw_watch_write(true) is called.
  // handle_read(fd, "", false) is dispatched on EV_READ.
  // handle_write(fd)          is dispatched on EV_WRITE.
  evt_io(evutil_socket_t fd, rawfd_tag);

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

  // Create (or replace) a timer identified by timer_id.
  // When repeat is true the timer re-fires every tv seconds (EV_PERSIST);
  // otherwise it fires once.  handle_timeout(timer_id) is called on expiry.
  void arm_timer(int timer_id, const struct timeval &tv, bool repeat = false);
  void disarm_timer(int timer_id);
  virtual std::int32_t handle_timeout(int timer_id);

protected:
  // Enable or disable the EV_WRITE watcher for a raw-fd connection.
  // Call raw_watch_write(true)  when there is pending outgoing data.
  // Call raw_watch_write(false) once the send queue is empty to avoid
  // spinning on a writable fd.
  void raw_watch_write(bool enable);

  // For inbound peers constructed from a pre-built bufferevent (plain or TLS).
  // vpn_peer uses this after vpn_server calls wrap_accepted().
  evt_io(struct bufferevent *bev, const std::string &peer_host)
      : m_from_host(peer_host),
        m_buffer_evt_p(bev),
        m_listener_p(nullptr) {
    bufferevent_setcb(bev, client_read_cb, client_write_cb,
                      client_event_cb, this);
    bufferevent_enable(bev, EV_READ | EV_WRITE | EV_PERSIST);
  }

  // Wraps an accepted fd in a bufferevent appropriate for this listener.
  // Returns a TLS-accepting bev when this server was created with a TLS ctx;
  // returns a plain socket bev otherwise.  Used by server-side handle_connect.
  struct bufferevent *wrap_accepted(evutil_socket_t fd) const {
    if (m_ssl_ctx) {
      SSL *ssl = SSL_new(m_ssl_ctx.get());
      return bufferevent_openssl_socket_new(evt_base::instance().get(), fd, ssl,
                                             BUFFEREVENT_SSL_ACCEPTING,
                                             BEV_OPT_CLOSE_ON_FREE);
    }
    return bufferevent_socket_new(evt_base::instance().get(), fd,
                                   BEV_OPT_CLOSE_ON_FREE);
  }

  bool has_tls() const noexcept { return m_ssl_ctx != nullptr; }

public:
  virtual ~evt_io() {
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

  void init_listener(const std::string &host, uint16_t port) {
    struct addrinfo *result;
    struct sockaddr_in self_addr;
    if (!getaddrinfo(host.data(), std::to_string(port).c_str(), nullptr, &result)) {
      self_addr = *reinterpret_cast<struct sockaddr_in *>(result->ai_addr);
      freeaddrinfo(result);
      m_listener_p.reset(evconnlistener_new_bind(
          evt_base::instance().get(), server_accept_cb, this,
          LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE | LEV_OPT_CLOSE_ON_EXEC,
          32, reinterpret_cast<struct sockaddr *>(&self_addr), sizeof(self_addr)));
    }
  }

  struct timer_ctx { evt_io *self; int id; };
  static void timer_dispatch_cb(evutil_socket_t, short, void *arg);

  std::string m_from_host;
  std::unique_ptr<struct bufferevent, custom_deleter> m_buffer_evt_p;
  std::unique_ptr<struct evconnlistener, custom_deleter_listener> m_listener_p;
  ssl_ctx_ptr m_ssl_ctx;
  // Raw-fd mode (rawfd_tag constructor): read + write events, no bufferevent.
  evt_timer m_raw_read_ev;
  evt_timer m_raw_write_ev;
  // Named timers: keyed by caller-chosen integer ID.
  std::map<int, std::pair<evt_timer, timer_ctx>> m_timers;
};

struct run_evt_loop {

  int operator()() {
    event_base_dispatch(evt_base::instance().get());
    return (0);
  }
};
#endif /* __framework_hpp__ */
