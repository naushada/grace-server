#ifndef __vpn_server_hpp__
#define __vpn_server_hpp__

#include "framework.hpp"
#include "tls_config.hpp"
#include "vpn_types.hpp"

#include <set>
#include <string>
#include <unordered_map>

class vpn_peer;

// IP address pool supporting any IPv4 range — enables 1000+ concurrent clients.
class ip_pool {
public:
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

// VPN tunnel server.  Accepts clients, assigns virtual IPs from the pool, and
// creates one vpn_peer per connection.  TLS is optional.
//
// When mqtt_sub is enabled, the broker config is forwarded to each peer on
// connect so the peer can subscribe to its own "fwd/<vip>" topic and handle
// gNMI requests from the CLI independently.
class vpn_server : public evt_io {
public:
  using handle_t   = int32_t;
  using peer_map_t = std::unordered_map<handle_t, std::unique_ptr<vpn_peer>>;

  vpn_server(const std::string  &host,
                 uint16_t            port,
                 const std::string  &pool_start = "10.8.0.2",
                 const std::string  &pool_end   = "10.8.0.254",
                 const tls_config   &tls        = {},
                 const std::string  &server_ip  = "10.8.0.1",
                 const std::string  &netmask    = "255.255.255.0",
                 const mqtt_sub_cfg &mqtt_sub   = {});

  virtual ~vpn_server();

  std::int32_t handle_connect(const handle_t &channel,
                               const std::string &peer_host) override;
  std::int32_t handle_close(const handle_t &channel) override;

  ip_pool &pool() { return m_pool; }

private:
  class server_tun_io;
  friend class server_tun_io;

  int  open_server_tun(const std::string &server_ip);
  void manage_client_route(const std::string &client_ip, bool add);

  ip_pool                        m_pool;
  peer_map_t                     m_peers;
  std::string                    m_netmask;
  std::string                    m_server_tun_name;
  std::unique_ptr<server_tun_io> m_server_tun_io;
  int                            m_server_tun_fd{-1};
  mqtt_sub_cfg                   m_mqtt_cfg;  // forwarded to each peer on connect
};

#endif // __vpn_server_hpp__
