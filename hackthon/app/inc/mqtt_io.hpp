#ifndef __mqtt_io_hpp__
#define __mqtt_io_hpp__

#include "framework.hpp"
#include <mosquitto.h>
#include <string>

// Integrates a libmosquitto MQTT connection with the libevent framework.
//
// The mosquitto socket is wrapped via the rawfd_tag evt_io constructor so all
// reads and writes are event-driven (no polling timer):
//   handle_read()  → mosquitto_loop_read()
//   handle_write() → mosquitto_loop_write()
//
// A 1-second misc timer calls mosquitto_loop_misc() for MQTT keepalive/ping.
//
// Usage:
//   auto io = std::make_unique<mqtt_io>(
//                 "broker", 1883, "client-id",
//                 my_message_callback, userdata);
//   io->subscribe("topic/#");
//   run_evt_loop{}();   // io stays in scope for the loop duration
class mqtt_io : public evt_io {
public:
  // Connects async to the MQTT broker and registers the socket with the
  // libevent loop.  message_cb (optional) is invoked for each arriving
  // message; userdata is forwarded to every callback as the void* arg.
  mqtt_io(const std::string &host, uint16_t port,
          const std::string &client_id,
          void (*message_cb)(struct mosquitto *, void *,
                             const struct mosquitto_message *) = nullptr,
          void *userdata = nullptr);
  ~mqtt_io() override;

  // Queue an MQTT SUBSCRIBE and arm the write event to flush it.
  void subscribe(const std::string &topic, int qos = 0);

  // Queue an MQTT PUBLISH and arm the write event to flush it.
  // retain=true stores the last value on the broker so late subscribers
  // receive it immediately on subscribe.  Returns a MOSQ_ERR_* code.
  int publish(const std::string &topic, const void *payload, int payloadlen,
              int qos = 0, bool retain = false);

  // evt_io overrides — called by framework when the mosquitto socket is ready.
  std::int32_t handle_read(const std::int32_t &, const std::string &,
                            const bool &) override;
  std::int32_t handle_write(const std::int32_t &) override;
  // Periodic 1-second tick: drives MQTT keepalive/ping via mosquitto_loop_misc.
  std::int32_t handle_timeout(int timer_id) override;

private:
  static constexpr int TIMER_MISC = 0;

  // Creates the mosquitto instance and calls connect_async().  Called from
  // the constructor's base-class initialiser (before mqtt_io members exist);
  // stores the mosquitto* in a static scratch so the constructor body can
  // retrieve it.  Returns the underlying socket fd.
  static evutil_socket_t init_mosq(
      const std::string &host, uint16_t port, const std::string &client_id,
      void (*message_cb)(struct mosquitto *, void *,
                         const struct mosquitto_message *),
      void *userdata);

  struct mosquitto *m_mosq{nullptr};
};

#endif // __mqtt_io_hpp__
