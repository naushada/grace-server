#ifndef __openvpn_client_hpp__
#define __openvpn_client_hpp__

#include "framework.hpp"

#include <cstdint>
#include <ctime>
#include <string>

// Persistent outbound VPN tunnel client.
//
// Inherits evt_io (outbound TCP constructor) — same pattern as gnmi_connection
// and openvpn_tunnel_client, but long-lived.
//
// Hook flow:
//   BEV_EVENT_CONNECTED → handle_connect(): log, nothing sent (server speaks first)
//   client_read_cb      → handle_read():
//       TYPE_IP_ASSIGN  → store IP, create tunX interface, write status Lua
//       TYPE_DATA       → write raw bytes to the TUN fd
//   BEV_EVENT_EOF/ERROR → handle_close(): write status "Down", close TUN fd
//   BEV_EVENT_TIMEOUT   → handle_event(): write status "Down", close TUN fd
//
// Frame format (shared with openvpn_server/openvpn_peer):
//   [type:1][length:4 BE][payload:N]
//     0x01  IP_ASSIGN   server→client  ASCII virtual IP string
//     0x02  DATA        bidirectional  raw tunnel bytes
//     0x03  DISCONNECT  client→server  graceful close

class openvpn_client : public evt_io {
public:
  static constexpr uint8_t TYPE_IP_ASSIGN  = 0x01;
  static constexpr uint8_t TYPE_DATA       = 0x02;
  static constexpr uint8_t TYPE_DISCONNECT = 0x03;
  static constexpr size_t  HEADER_LEN      = 5; // 1 (type) + 4 (length BE)

  // host/port   — openvpn_server to connect to
  // status_file — path where the Lua status table is written
  //
  // The TUN interface name is NOT a parameter: we pass an empty ifr_name to
  // the TUNSETIFF ioctl and let the kernel assign the next free tunX.
  // The actual name is stored in tun_if() after IP_ASSIGN is received.
  openvpn_client(const std::string &host, uint16_t port,
                  std::string status_file = "/tmp/vpn_status.lua");

  virtual ~openvpn_client();

  const std::string &assigned_ip()  const { return m_assigned_ip; }
  // Kernel-assigned interface name ("tun1", "tun2", …); empty until IP_ASSIGN.
  const std::string &tun_if()       const { return m_tun_name; }
  bool               ip_assigned()  const { return m_ip_assigned; }

  // -------------------------------------------------------------------------
  // evt_io hook overrides
  // -------------------------------------------------------------------------
  std::int32_t handle_connect(const std::int32_t &channel,
                               const std::string &peer_host) override;
  std::int32_t handle_read(const std::int32_t &channel,
                            const std::string &data,
                            const bool &dry_run) override;
  std::int32_t handle_close(const std::int32_t &channel) override;
  std::int32_t handle_event(const std::int32_t &channel,
                             const std::uint16_t &event) override;
  std::int32_t handle_write(const std::int32_t &channel) override;

  // -------------------------------------------------------------------------
  // Static helpers — exposed for unit-testability
  // -------------------------------------------------------------------------

  // Build a 5-byte-prefixed frame: [type:1][length:4 BE][payload].
  static std::string encode_frame(uint8_t type, const std::string &payload);

  // Try to decode one complete frame starting at buf[offset].
  // Returns true and fills out_* on success; leaves buf unchanged on failure.
  static bool try_decode_frame(const std::string &buf, size_t offset,
                                uint8_t &out_type, std::string &out_payload,
                                size_t &out_consumed);

  // Write (or overwrite) a Lua status file with the current VPN state.
  //   path       — file to write
  //   service_ip — virtual IP assigned by the server, or "" if down
  //   tun_if     — kernel-assigned TUN interface name, or "" if down
  //   status     — "Connected" or "Down"
  //   timestamp  — UTC seconds since epoch (use std::time(nullptr))
  static void write_status_lua(const std::string &path,
                                const std::string &service_ip,
                                const std::string &tun_if,
                                const std::string &status,
                                std::time_t        timestamp);

private:
  // Open /dev/net/tun (Linux) with an empty ifr_name so the kernel picks
  // the next free tunX.  Stores the kernel-assigned name in m_tun_name.
  // Returns the open fd on success, -1 on failure or non-Linux platform.
  int  open_tun();
  void close_tun();

  // Reassemble and dispatch complete frames from m_recv_buf.
  // Returns the number of bytes consumed.
  size_t process_frames();

  // Wrap payload in a frame and transmit via bufferevent.
  void send_frame(uint8_t type, const std::string &payload);

  std::string m_tun_name;
  std::string m_status_file;
  std::string m_assigned_ip;
  std::string m_recv_buf;
  int         m_tun_fd{-1};
  bool        m_ip_assigned{false};
};

#endif // __openvpn_client_hpp__
