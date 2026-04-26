#ifndef __openvpn_client_hpp__
#define __openvpn_client_hpp__

#include "framework.hpp"
#include "tls_config.hpp"

#include <cstdint>
#include <ctime>
#include <string>

// Persistent outbound VPN tunnel client.
//
// TLS is optional: when tls.enabled is true the outbound bufferevent is built
// as a bufferevent_openssl_socket, passed to the protected evt_io(bev, host)
// constructor via the static make_bev() helper in the initialiser list.
//
// Lua status file is written via lua_file::write_table (single Lua write path).
class openvpn_client : public evt_io {
public:
  static constexpr uint8_t TYPE_IP_ASSIGN  = 0x01;
  static constexpr uint8_t TYPE_DATA       = 0x02;
  static constexpr uint8_t TYPE_DISCONNECT = 0x03;
  static constexpr size_t  HEADER_LEN      = 5;

  openvpn_client(const std::string &host, uint16_t port,
                  std::string        status_file = "/tmp/vpn_status.lua",
                  const tls_config  &tls         = {});

  virtual ~openvpn_client();

  const std::string &assigned_ip() const { return m_assigned_ip; }
  const std::string &tun_if()      const { return m_tun_name; }
  bool               ip_assigned() const { return m_ip_assigned; }

  std::int32_t handle_connect(const std::int32_t &channel,
                               const std::string &peer_host) override;
  std::int32_t handle_read(const std::int32_t &channel,
                            const std::string &data,
                            const bool &dry_run) override;
  std::int32_t handle_close(const std::int32_t &channel) override;
  std::int32_t handle_event(const std::int32_t &channel,
                             const std::uint16_t &event) override;
  std::int32_t handle_write(const std::int32_t &channel) override;

  // --------------------------------------------------------------------------
  // Static helpers — exposed for unit-testability
  // --------------------------------------------------------------------------
  static std::string encode_frame(uint8_t type, const std::string &payload);
  static bool try_decode_frame(const std::string &buf, size_t offset,
                                uint8_t &out_type, std::string &out_payload,
                                size_t &out_consumed);

  // All Lua output is routed through lua_file::write_table.
  static void write_status_lua(const std::string &path,
                                const std::string &service_ip,
                                const std::string &tun_if,
                                const std::string &status,
                                std::time_t        timestamp);

private:
  class tun_io;
  friend class tun_io;

  int    open_tun();
  void   assign_tun_ip(const std::string &ip);
  void   close_tun();
  size_t process_frames();
  void   send_frame(uint8_t type, const std::string &payload);

  std::string              m_tun_name;
  std::string              m_status_file;
  std::string              m_assigned_ip;
  std::string              m_recv_buf;
  std::unique_ptr<tun_io>  m_tun_io;
  int                      m_tun_fd{-1};
  bool                     m_ip_assigned{false};
};

#endif // __openvpn_client_hpp__
