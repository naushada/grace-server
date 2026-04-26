#ifndef __gnmi_client_hpp__
#define __gnmi_client_hpp__

#include "tls_config.hpp"

#include <cstdint>
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

  // Blocking unary gRPC call — plain TCP or TLS depending on tls.enabled.
  //   host       — IPv4/IPv6 address or hostname of the target
  //   port       — TCP port (gNMI default: 58989)
  //   rpc_path   — "/package.Service/Method" e.g. "/gnmi.gNMI/Get"
  //   request_pb — serialised request protobuf bytes (NOT gRPC-framed)
  //   tls        — TLS configuration; disabled by default (plain TCP)
  //
  // Returns a response with grpc_status=-1 on transport/timeout errors.
  static response call(const std::string &host, uint16_t port,
                       const std::string &rpc_path,
                       const std::string &request_pb,
                       const tls_config &tls = {});
};

#endif // __gnmi_client_hpp__
