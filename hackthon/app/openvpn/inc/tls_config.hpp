#ifndef __tls_config_hpp__
#define __tls_config_hpp__

#include <openssl/ssl.h>
#include <iostream>
#include <string>

// TLS configuration shared by openvpn_server and openvpn_client.
// Build an SSL_CTX once at startup and pass it around; the context is
// shared across all connections.
struct tls_config {
  bool        enabled{false};
  std::string cert_file; // PEM certificate (server + client mutual-TLS)
  std::string key_file;  // PEM private key
  std::string ca_file;   // CA certificate for peer verification

  // Build a server-side SSL_CTX (TLS_server_method).
  // Returns nullptr when disabled or on configuration error.
  SSL_CTX *build_server_ctx() const {
    if (!enabled) return nullptr;
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) return nullptr;
    if (SSL_CTX_use_certificate_file(ctx, cert_file.c_str(),
                                      SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(),
                                     SSL_FILETYPE_PEM) != 1) {
      std::cerr << "[tls] server ctx: cert/key load failed\n";
      SSL_CTX_free(ctx);
      return nullptr;
    }
    if (!ca_file.empty())
      SSL_CTX_load_verify_locations(ctx, ca_file.c_str(), nullptr);
    return ctx;
  }

  // Build a client-side SSL_CTX (TLS_client_method).
  // Returns nullptr when disabled or on configuration error.
  SSL_CTX *build_client_ctx() const {
    if (!enabled) return nullptr;
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    if (!ctx) return nullptr;
    if (!ca_file.empty())
      SSL_CTX_load_verify_locations(ctx, ca_file.c_str(), nullptr);
    if (!cert_file.empty() && !key_file.empty()) {
      if (SSL_CTX_use_certificate_file(ctx, cert_file.c_str(),
                                        SSL_FILETYPE_PEM) != 1 ||
          SSL_CTX_use_PrivateKey_file(ctx, key_file.c_str(),
                                       SSL_FILETYPE_PEM) != 1) {
        std::cerr << "[tls] client ctx: cert/key load failed\n";
        SSL_CTX_free(ctx);
        return nullptr;
      }
    }
    return ctx;
  }
};

#endif // __tls_config_hpp__
