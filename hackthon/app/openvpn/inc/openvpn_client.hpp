#ifndef __openvpn_client_hpp__
#define __openvpn_client_hpp__

#include "framework.hpp"
#include "tls_config.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// openvpn_client — wraps the standard `openvpn --client` binary.
//
// Spawns the system openvpn binary as a child process and reads its combined
// stdout+stderr through a pipe registered with the shared libevent event base
// via evt_io(pipe_fd, name).  All I/O is non-blocking and event-driven —
// no threads, no polling loops.
//
// After the child reports "Initialization Sequence Completed" the tunnel is
// considered up.  The assigned virtual IP is extracted from the openvpn log
// ("local <vip>" in the ip-addr-add line, or ifconfig_local env var).
//
// On tunnel-up, iptables PREROUTING DNAT rules are installed for every port
// in fwd_ports, forwarding <vip>:<port> → <fwd_host>:<port> so that gNMI
// (58989), HTTPS (443) and HTTP (80) traffic arriving at the VIP reaches
// the corresponding local service without requiring the openvpn process to
// handle any application-level I/O itself.
//
// The rules are torn down when the object is destroyed or the child exits.
class openvpn_client {
public:
  // fwd_ports: traffic on these ports at <vip> will be DNAT'd to fwd_host.
  // fwd_host:  destination host for the DNAT rules (default: 127.0.0.1).
  openvpn_client(const std::string     &server_host,
                 uint16_t               server_port,
                 const tls_config      &tls        = {},
                 std::string            status_file = "/run/openvpn_status.lua",
                 std::vector<uint16_t>  fwd_ports   = {80, 443, 58989},
                 std::string            fwd_host    = "127.0.0.1");

  ~openvpn_client();

  bool               tunnel_up()   const { return m_tunnel_up; }
  const std::string &assigned_ip() const { return m_assigned_ip; }

private:
  class proc_io;
  friend class proc_io;

  void parse_line(const std::string &line);
  void setup_nat(const std::string &vip);
  void teardown_nat(const std::string &vip);

  pid_t                    m_pid{-1};
  int                      m_pipe_r{-1};
  bool                     m_tunnel_up{false};
  std::string              m_assigned_ip;
  std::string              m_recv_buf;
  std::string              m_status_file;
  std::string              m_fwd_host;
  std::vector<uint16_t>    m_fwd_ports;
  std::unique_ptr<proc_io> m_proc_io;
};

#endif // __openvpn_client_hpp__
