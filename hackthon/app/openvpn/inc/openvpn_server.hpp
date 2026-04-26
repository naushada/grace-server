#ifndef __openvpn_server_hpp__
#define __openvpn_server_hpp__

#include "framework.hpp"
#include "tls_config.hpp"

#include <set>
#include <string>
#include <unordered_map>

class openvpn_peer;

// IP address pool supporting any IPv4 range — enables 1000+ concurrent clients.
//
// Scalability note: libevent uses epoll on Linux, handling 65 K+ concurrent fds
// in a single thread (C10K is solved at the I/O layer).  The only hard cap was
// this pool being limited to a /24 (253 addresses).  Switching to uint32_t host
// addresses lets operators configure e.g. "10.8.0.2"–"10.9.255.254" giving
// 131 070 assignable virtual IPs with ~12 bytes per entry.
class ip_pool {
public:
  // start_ip / end_ip as dotted-decimal strings, e.g. "10.8.0.2", "10.8.0.254"
  // For 1000+ clients use a /22 or larger: "10.8.0.2" → "10.11.255.254"
  ip_pool(const std::string &start_ip = "10.8.0.2",
          const std::string &end_ip   = "10.8.0.254");

  std::string assign(int32_t channel); // "" if exhausted
  void        release(int32_t channel);
  std::string get(int32_t channel) const;
  int32_t     find_channel(const std::string &ip) const; // -1 if not found
  size_t      available() const { return m_free.size(); }

private:
  static uint32_t    to_u32(const std::string &ip);
  static std::string to_str(uint32_t addr);

  std::set<uint32_t>                    m_free;
  std::unordered_map<int32_t, uint32_t> m_assigned;
};

// TCP tunnel server.  Inherits the server-side evt_io constructor
// (evconnlistener); creates one openvpn_peer per accepted connection and
// assigns a virtual IP from the pool.  TLS is optional: when enabled each
// accepted fd is wrapped in bufferevent_openssl_socket_new before the peer
// object is constructed.
class openvpn_server : public evt_io {
public:
  using handle_t   = int32_t;
  using peer_map_t = std::unordered_map<handle_t, std::unique_ptr<openvpn_peer>>;

  openvpn_server(const std::string &host, uint16_t port,
                 const std::string &pool_start  = "10.8.0.2",
                 const std::string &pool_end    = "10.8.0.254",
                 const tls_config  &tls         = {},
                 const std::string &server_ip   = "10.8.0.1");

  virtual ~openvpn_server();

  std::int32_t handle_connect(const handle_t &channel,
                               const std::string &peer_host) override;
  std::int32_t handle_close(const handle_t &channel) override;

  ip_pool &pool() { return m_pool; }

private:
  class server_tun_io;
  friend class server_tun_io;

  int open_server_tun(const std::string &server_ip);

  ip_pool                       m_pool;
  peer_map_t                    m_peers;
  std::unique_ptr<server_tun_io> m_server_tun_io;
  int                           m_server_tun_fd{-1};
};

#endif // __openvpn_server_hpp__
