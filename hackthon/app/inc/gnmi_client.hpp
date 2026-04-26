#ifndef __gnmi_client_hpp__
#define __gnmi_client_hpp__

#include <cstdint>
#include <string>

// gnmi_client makes a blocking unary gRPC call using the libevent bufferevent
// interface (the same event base used by the rest of the process).
//
// No raw POSIX sockets — all I/O goes through bufferevent_socket_new /
// bufferevent_socket_connect_hostname / bufferevent_write, consistent with
// how evt_io operates in framework.hpp.
class gnmi_client {
public:
  struct response {
    int grpc_status{-1};      // -1 = transport error, 0 = OK
    std::string grpc_message; // grpc-message trailer (may be empty)
    std::string body_pb;      // decoded response protobuf bytes (unframed)

    bool ok() const { return grpc_status == 0; }
  };

  // Blocking unary gRPC call.  Internally creates a bufferevent on
  // evt_base::instance(), performs the HTTP/2 handshake and gRPC exchange,
  // then runs event_base_loop(EVLOOP_ONCE) until the response arrives.
  //
  //   host       — IPv4/IPv6 address or hostname of the target
  //   port       — TCP port (gNMI default: 9339)
  //   rpc_path   — "/package.Service/Method" e.g. "/gnmi.gNMI/Get"
  //   request_pb — serialised request protobuf bytes (NOT gRPC-framed)
  //
  // Returns a response with grpc_status=-1 on transport/timeout errors.
  static response call(const std::string &host, uint16_t port,
                       const std::string &rpc_path,
                       const std::string &request_pb);
};

#endif // __gnmi_client_hpp__
