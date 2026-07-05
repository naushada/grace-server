#ifndef __gnmi_client_hpp__
#define __gnmi_client_hpp__

#include "tls_config.hpp"

#include <cstdint>
#include <functional>
#include <string>

// gnmi_client makes a blocking unary gRPC call using the libevent bufferevent
// interface (the same event base used by the rest of the process).
// All socket I/O goes through evt_io (framework.hpp) — no raw POSIX sockets.
class gnmi_client {
public:
  struct response {
    int grpc_status{-1};      // -1 = transport error, 0 = OK
    std::string grpc_message; // grpc-message trailer (may be empty)
    std::string body_pb;      // decoded response protobuf bytes (unframed)

    bool ok() const { return grpc_status == 0; }
  };

  // Blocking unary gRPC call — safe to call OUTSIDE the running event loop
  // (phased approach: call from main before event_base_dispatch).
  static response call(const std::string &host, uint16_t port,
                       const std::string &rpc_path,
                       const std::string &request_pb,
                       const tls_config &tls = {});

  // Called exactly once when the exchange completes (success or error).
  using response_cb = std::function<void(const response &)>;

  // Called for each streamed SubscribeResponse (serialised protobuf bytes) as
  // it arrives on a Subscribe stream.
  using notification_cb = std::function<void(const std::string &response_pb)>;

  // Non-blocking push — safe to call from INSIDE a running event loop.
  // Creates a gnmi_connection registered with the shared event base and lets
  // event_base_dispatch drive it.  Completed connections are lazily freed on
  // the next push_async call.  on_done (optional) is invoked exactly once
  // when the response arrives or an error terminates the exchange.
  static void push_async(const std::string &host, uint16_t port,
                         const std::string &rpc_path,
                         const std::string &request_pb,
                         const tls_config &tls = {},
                         response_cb on_done = {});

  // Open a gNMI Subscribe stream. on_notif fires for each SubscribeResponse as
  // it streams in; on_done fires once when the stream ends (or errors). Safe to
  // call from inside the running event loop; the connection lives until the
  // stream closes. Returns immediately.
  static void subscribe_async(const std::string &host, uint16_t port,
                              const std::string &request_pb,
                              const tls_config &tls, notification_cb on_notif,
                              response_cb on_done = {});
};

#endif // __gnmi_client_hpp__
