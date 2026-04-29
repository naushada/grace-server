#ifndef __tls_config_hpp__
#define __tls_config_hpp__

// ssl_ctx_ptr / ssl_ctx_deleter live in framework.hpp (alongside other RAII
// wrappers) so that evt_io can use them without depending on this header.
#include "framework.hpp"

#include <iostream>
#include <string>

// TLS configuration shared by vpn_server and vpn_client.
// Build an SSL_CTX once at startup and pass it around; the context is
// shared across all connections.
struct tls_config {
  bool        enabled{false};
  std::string cert_file; // PEM certificate (server + client mutual-TLS)
  std::string key_file;  // PEM private key
  std::string ca_file;   // CA certificate for peer verification

  // Build a server-side SSL_CTX (TLS_server_method).
  // Returns empty unique_ptr when disabled or on configuration error.
  ssl_ctx_ptr build_server_ctx() const {
    if (!enabled) return nullptr;
    ssl_ctx_ptr ctx{SSL_CTX_new(TLS_server_method())};
    if (!ctx) return nullptr;
    if (SSL_CTX_use_certificate_file(ctx.get(), cert_file.c_str(),
                                      SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx.get(), key_file.c_str(),
                                     SSL_FILETYPE_PEM) != 1) {
      std::cerr << "[tls] server ctx: cert/key load failed\n";
      return nullptr;
    }
    if (!ca_file.empty()) {
      SSL_CTX_load_verify_locations(ctx.get(), ca_file.c_str(), nullptr);
      // Require client to present a certificate signed by the CA.
      // CN-based authorisation is then enforced in vpn_peer::handle_connect.
      SSL_CTX_set_verify(ctx.get(),
                         SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                         nullptr);
    }
    return ctx;
  }

  // Build a client-side SSL_CTX (TLS_client_method).
  // Returns empty unique_ptr when disabled or on configuration error.
  ssl_ctx_ptr build_client_ctx() const {
    if (!enabled) return nullptr;
    ssl_ctx_ptr ctx{SSL_CTX_new(TLS_client_method())};
    if (!ctx) return nullptr;
    if (!ca_file.empty())
      SSL_CTX_load_verify_locations(ctx.get(), ca_file.c_str(), nullptr);
    if (!cert_file.empty() && !key_file.empty()) {
      if (SSL_CTX_use_certificate_file(ctx.get(), cert_file.c_str(),
                                        SSL_FILETYPE_PEM) != 1 ||
          SSL_CTX_use_PrivateKey_file(ctx.get(), key_file.c_str(),
                                       SSL_FILETYPE_PEM) != 1) {
        std::cerr << "[tls] client ctx: cert/key load failed\n";
        return nullptr;
      }
    }
    return ctx;
  }
};

#endif // __tls_config_hpp__
