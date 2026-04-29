#ifndef __vpn_tunnel_client_cpp__
#define __vpn_tunnel_client_cpp__

#include "vpn_tunnel_client.hpp"
#include "framework.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>

// ---------------------------------------------------------------------------
// tunnel_session — private client-side connection, mirrors gnmi_connection.
//
// Hook flow:
//   BEV_EVENT_CONNECTED → handle_connect():  nothing to send, server speaks first
//   client_read_cb      → handle_read():     parse IP_ASSIGN frame, set m_done
//   BEV_EVENT_EOF/ERROR → handle_close():   set m_done on premature close
//   BEV_EVENT_TIMEOUT   → handle_event():   set m_done on timeout
// ---------------------------------------------------------------------------

class tunnel_session : public evt_io {
public:
  static constexpr uint8_t TYPE_IP_ASSIGN = 0x01;
  static constexpr size_t  HEADER_LEN     = 5;

  tunnel_session(const std::string &host, uint16_t port)
      : evt_io(host, port, /*outbound=*/true) {}

  bool        done()        const { return m_done; }
  bool        ok()          const { return m_ok;   }
  std::string assigned_ip() const { return m_assigned_ip; }
  std::string message()     const { return m_message; }

  std::int32_t handle_connect(const std::int32_t & /*channel*/,
                               const std::string & /*peer*/) override {
    // Server sends IP_ASSIGN first — nothing to do on connect.
    return 0;
  }

  std::int32_t handle_read(const std::int32_t & /*channel*/,
                            const std::string &data,
                            const bool &dry_run) override {
    if (dry_run)
      return 0;

    m_recv_buf.append(data);
    if (m_recv_buf.size() < HEADER_LEN)
      return static_cast<int32_t>(data.size());

    const uint8_t type = static_cast<uint8_t>(m_recv_buf[0]);
    uint32_t len_be = 0;
    std::memcpy(&len_be, m_recv_buf.data() + 1, 4);
    const uint32_t len = ntohl(len_be);

    if (m_recv_buf.size() < HEADER_LEN + len)
      return static_cast<int32_t>(data.size()); // wait for rest

    if (type == TYPE_IP_ASSIGN) {
      m_assigned_ip = m_recv_buf.substr(HEADER_LEN, len);
      m_ok          = true;
      std::cout << "[vpn_tunnel] assigned IP=" << m_assigned_ip << "\n";
    } else {
      m_message = "unexpected frame type from server";
    }
    m_done = true;
    return static_cast<int32_t>(HEADER_LEN + len);
  }

  std::int32_t handle_close(const std::int32_t & /*channel*/) override {
    if (!m_done) {
      m_message = "connection closed before IP_ASSIGN";
      m_done    = true;
    }
    return 0;
  }

  std::int32_t handle_event(const std::int32_t & /*channel*/,
                             const std::uint16_t & /*events*/) override {
    std::cerr << "[vpn_tunnel] timed out\n";
    m_message = "timeout";
    m_done    = true;
    return 0;
  }

  std::int32_t handle_write(const std::int32_t & /*channel*/) override {
    return 0;
  }

private:
  bool        m_done{false};
  bool        m_ok{false};
  std::string m_assigned_ip;
  std::string m_message;
  std::string m_recv_buf;
};

// ---------------------------------------------------------------------------
// vpn_tunnel_client::connect
// ---------------------------------------------------------------------------

vpn_tunnel_client::result
vpn_tunnel_client::connect(const std::string &host, uint16_t port) {
  tunnel_session sess(host, port);
  while (!sess.done())
    event_base_loop(evt_base::instance().get(), EVLOOP_ONCE);
  return {sess.ok(), sess.assigned_ip(), sess.message()};
}

#endif // __vpn_tunnel_client_cpp__
