#ifndef __mqtt_io_cpp__
#define __mqtt_io_cpp__

#include "mqtt_io.hpp"
#include <iostream>

// Scratch storage for the mosquitto* created in init_mosq().
// init_mosq() is called as a base-class constructor argument before any
// mqtt_io member exists; the constructor body retrieves it from here.
// Single-threaded event loop — a plain static is safe.
static struct mosquitto *s_mosq_scratch{nullptr};

evutil_socket_t mqtt_io::init_mosq(
    const std::string &host, uint16_t port, const std::string &client_id,
    void (*message_cb)(struct mosquitto *, void *,
                       const struct mosquitto_message *),
    void *userdata) {
  mosquitto_lib_init();
  s_mosq_scratch = mosquitto_new(client_id.c_str(), true, userdata);
  if (!s_mosq_scratch) {
    std::cerr << "[mqtt_io] mosquitto_new failed\n";
    return -1;
  }
  if (message_cb)
    mosquitto_message_callback_set(s_mosq_scratch, message_cb);
  const int rc = mosquitto_connect_async(s_mosq_scratch, host.c_str(),
                                          static_cast<int>(port), 60);
  if (rc != MOSQ_ERR_SUCCESS) {
    std::cerr << "[mqtt_io] connect_async to " << host << ":" << port
              << " failed: " << mosquitto_strerror(rc) << '\n';
    mosquitto_destroy(s_mosq_scratch);
    s_mosq_scratch = nullptr;
    return -1;
  }
  return mosquitto_socket(s_mosq_scratch);
}

mqtt_io::mqtt_io(const std::string &host, uint16_t port,
                 const std::string &client_id,
                 void (*message_cb)(struct mosquitto *, void *,
                                    const struct mosquitto_message *),
                 void *userdata)
    : evt_io(init_mosq(host, port, client_id, message_cb, userdata),
             rawfd_tag{}) {
  m_mosq = s_mosq_scratch;
  s_mosq_scratch = nullptr;
  if (!m_mosq) return;

  // CONNECT packet queued by connect_async — arm write event to flush it.
  if (mosquitto_want_write(m_mosq))
    raw_watch_write(true);

  arm_timer(TIMER_MISC, {1, 0}, /*repeat=*/true);

  std::cout << "[mqtt_io] connecting to " << host << ":" << port
            << " as '" << client_id << "'\n";
}

mqtt_io::~mqtt_io() {
  if (m_mosq) {
    mosquitto_disconnect(m_mosq);
    mosquitto_destroy(m_mosq);
    mosquitto_lib_cleanup();
    m_mosq = nullptr;
  }
}

// EV_READ fires → drive mosquitto reads; enable write if data queued.
std::int32_t mqtt_io::handle_read(const std::int32_t &, const std::string &,
                                   const bool &) {
  if (!m_mosq) return -1;
  mosquitto_loop_read(m_mosq, 1);
  if (mosquitto_want_write(m_mosq))
    raw_watch_write(true);
  return 0;
}

// EV_WRITE fires → flush outgoing data; disarm write event when done.
std::int32_t mqtt_io::handle_write(const std::int32_t &) {
  if (!m_mosq) return -1;
  mosquitto_loop_write(m_mosq, 1);
  if (!mosquitto_want_write(m_mosq))
    raw_watch_write(false);
  return 0;
}

// 1-second repeating timer: MQTT keepalive, ping, reconnect logic.
std::int32_t mqtt_io::handle_timeout(int /*timer_id*/) {
  if (!m_mosq) return -1;
  mosquitto_loop_misc(m_mosq);
  return 0;
}

void mqtt_io::subscribe(const std::string &topic, int qos) {
  if (!m_mosq) return;
  mosquitto_subscribe(m_mosq, nullptr, topic.c_str(), qos);
  raw_watch_write(true); // flush SUBSCRIBE packet
}

int mqtt_io::publish(const std::string &topic, const void *payload,
                     int payloadlen, int qos, bool retain) {
  if (!m_mosq) return MOSQ_ERR_INVAL;
  const int rc = mosquitto_publish(m_mosq, nullptr, topic.c_str(),
                                    payloadlen, payload, qos, retain);
  if (rc == MOSQ_ERR_SUCCESS)
    raw_watch_write(true); // flush PUBLISH packet
  return rc;
}

#endif // __mqtt_io_cpp__
