#ifndef __openvpn_server_hpp__
#define __openvpn_server_hpp__

#include "framework.hpp"

#include <memory>
#include <set>
#include <string>
#include <unordered_map>

class openvpn_peer;

// Manages the pool of virtual IP addresses issued to VPN clients.
// Network is a /24 prefix string (e.g. "10.8.0"); host octets in [start, end]
// are kept in a free set and handed out one per accepted connection.
class ip_pool {
public:
  ip_pool(const std::string &network = "10.8.0",
          uint8_t start = 2, uint8_t end = 254);

  // Assign the next free IP to channel. Returns "" if pool is exhausted.
  std::string assign(int32_t channel);

  // Return the IP assigned to channel back to the free set.
  void release(int32_t channel);

  // Lookup the IP currently assigned to channel (empty if none).
  std::string get(int32_t channel) const;

private:
  std::string                          m_network;
  std::set<uint8_t>                    m_free;
  std::unordered_map<int32_t, uint8_t> m_assigned;
};

// TCP tunnel server — inherits the server-side evt_io constructor
// (evconnlistener) so incoming TCP connections trigger handle_connect().
// Creates one openvpn_peer per accepted connection and assigns an IP from
// the pool.  When a peer disconnects it calls back to handle_close() here
// so the entry is removed and the IP returned to the pool.
class openvpn_server : public evt_io {
public:
  using handle_t   = int32_t;
  using peer_map_t = std::unordered_map<handle_t, std::unique_ptr<openvpn_peer>>;

  openvpn_server(const std::string &host, uint16_t port,
                 const std::string &pool_network = "10.8.0",
                 uint8_t pool_start = 2, uint8_t pool_end = 254);

  virtual ~openvpn_server() { m_peers.clear(); }

  // Called by server_accept_cb on each new inbound TCP connection.
  std::int32_t handle_connect(const handle_t &channel,
                               const std::string &peer_host) override;

  // Called by openvpn_peer::handle_close() when a client disconnects.
  std::int32_t handle_close(const handle_t &channel) override;

  ip_pool &pool() { return m_pool; }

private:
  ip_pool     m_pool;
  peer_map_t  m_peers;
};

#endif // __openvpn_server_hpp__
