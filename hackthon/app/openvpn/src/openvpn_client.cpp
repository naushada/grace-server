#ifndef __openvpn_client_cpp__
#define __openvpn_client_cpp__

#include "openvpn_client.hpp"
#include "lua_engine.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <sstream>

#ifdef __linux__
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// proc_io — libevent reader for the pipe connected to openvpn's stdout+stderr
// ---------------------------------------------------------------------------

class openvpn_client::proc_io : public evt_io {
public:
  proc_io(evutil_socket_t fd, openvpn_client &owner)
      : evt_io(fd, "ovpn-client"), m_owner(owner) {}

  std::int32_t handle_read(const std::int32_t &, const std::string &data,
                            const bool &dry_run) override {
    if (dry_run) return 0;
    m_owner.m_recv_buf.append(data);
    size_t pos;
    while ((pos = m_owner.m_recv_buf.find('\n')) != std::string::npos) {
      std::string line = m_owner.m_recv_buf.substr(0, pos);
      m_owner.m_recv_buf.erase(0, pos + 1);
      // strip trailing \r
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) m_owner.parse_line(line);
    }
    return 0;
  }

  std::int32_t handle_close(const std::int32_t &) override {
    std::cerr << "[openvpn_client] child process stdout closed\n";
    return 0;
  }

private:
  openvpn_client &m_owner;
};

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

openvpn_client::openvpn_client(const std::string &server_host,
                                 uint16_t server_port,
                                 const tls_config &tls,
                                 std::string status_file,
                                 std::vector<uint16_t> fwd_ports,
                                 std::string fwd_host)
    : m_status_file(std::move(status_file)),
      m_fwd_host(std::move(fwd_host)),
      m_fwd_ports(std::move(fwd_ports)) {
#ifdef __linux__
  int pipefd[2];
  if (::pipe(pipefd) < 0) {
    std::cerr << "[openvpn_client] pipe: " << strerror(errno) << '\n';
    return;
  }

  // Build argv for the standard openvpn binary.
  std::vector<std::string> args_s = {
    "openvpn",
    "--client",
    "--dev", "tun",
    "--proto", "tcp-client",
    "--remote", server_host, std::to_string(server_port),
    "--nobind",
    "--persist-key",
    "--persist-tun",
    "--verb", "3",
    "--log", "/dev/stdout",
  };
  if (tls.enabled) {
    if (!tls.ca_file.empty())   { args_s.push_back("--ca");   args_s.push_back(tls.ca_file); }
    if (!tls.cert_file.empty()) { args_s.push_back("--cert"); args_s.push_back(tls.cert_file); }
    if (!tls.key_file.empty())  { args_s.push_back("--key");  args_s.push_back(tls.key_file); }
  } else {
    // Control channel TLS still requires cert/key/ca — use baked-in test certs.
    // Data channel: --data-ciphers none + --auth none → completely unencrypted.
    args_s.insert(args_s.end(), {
      "--ca",           "/app/certs/ca.pem",
      "--cert",         "/app/certs/client.pem",
      "--key",          "/app/certs/client.key",
      "--data-ciphers", "none",
      "--auth",         "none",
    });
  }

  std::vector<const char *> argv;
  for (const auto &s : args_s) argv.push_back(s.c_str());
  argv.push_back(nullptr);

  std::cout << "[openvpn_client] spawning: openvpn --client --remote "
            << server_host << " " << server_port
            << " tls=" << (tls.enabled ? "ON" : "OFF") << '\n';

  m_pid = ::fork();
  if (m_pid < 0) {
    std::cerr << "[openvpn_client] fork: " << strerror(errno) << '\n';
    ::close(pipefd[0]); ::close(pipefd[1]);
    return;
  }
  if (m_pid == 0) {
    // Child: wire stdout + stderr to the pipe write-end, then exec openvpn.
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::dup2(pipefd[1], STDERR_FILENO);
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    ::execvp("openvpn", const_cast<char *const *>(argv.data()));
    ::perror("execvp openvpn");
    ::_exit(1);
  }

  // Parent: close write end, register read end with libevent.
  ::close(pipefd[1]);
  m_pipe_r  = pipefd[0];
  m_proc_io = std::make_unique<proc_io>(m_pipe_r, *this);
#else
  (void)server_host; (void)server_port; (void)tls;
  std::cerr << "[openvpn_client] only supported on Linux\n";
#endif
}

openvpn_client::~openvpn_client() {
#ifdef __linux__
  teardown_nat(m_assigned_ip);
  m_proc_io.reset();
  if (m_pipe_r >= 0) { ::close(m_pipe_r); m_pipe_r = -1; }
  if (m_pid > 0) {
    ::kill(m_pid, SIGTERM);
    ::waitpid(m_pid, nullptr, WNOHANG);
    m_pid = -1;
  }
#endif
}

// ---------------------------------------------------------------------------
// Log parsing
// ---------------------------------------------------------------------------

// Extract the next whitespace-delimited token after 'key' in 'line'.
static std::string token_after(const std::string &line, const std::string &key) {
  const auto pos = line.find(key);
  if (pos == std::string::npos) return {};
  std::istringstream ss(line.substr(pos + key.size()));
  std::string tok;
  ss >> tok;
  // Trim any trailing non-IPv4 characters (comma, colon, etc.)
  const auto end = tok.find_first_not_of("0123456789.");
  return tok.substr(0, end);
}

static bool looks_like_ipv4(const std::string &s) {
  return s.size() >= 7 && s.find('.') != std::string::npos &&
         s.find_first_not_of("0123456789.") == std::string::npos;
}

void openvpn_client::parse_line(const std::string &line) {
  std::cout << "[ovpn] " << line << '\n';

  // ── Tunnel UP ─────────────────────────────────────────────────────────────
  if (line.find("Initialization Sequence Completed") != std::string::npos) {
    m_tunnel_up = true;
    std::cout << "[openvpn_client] tunnel UP  vip=" << m_assigned_ip << '\n';
    lua_file::write_table(m_status_file, "openvpn_status", {
      {"status",     "\"Connected\""},
      {"assigned_ip", "\"" + m_assigned_ip + "\""},
    });
    if (!m_assigned_ip.empty()) setup_nat(m_assigned_ip);
    return;
  }

  // ── VIP extraction ────────────────────────────────────────────────────────
  // Modern openvpn (ip route/addr): "ip addr add dev tun0 local 10.8.0.x peer ..."
  if (m_assigned_ip.empty() &&
      line.find("addr add") != std::string::npos &&
      line.find("local ") != std::string::npos) {
    const auto vip = token_after(line, "local ");
    if (looks_like_ipv4(vip)) { m_assigned_ip = vip; goto vip_found; }
  }

  // Legacy openvpn (ifconfig): "ifconfig tun0 10.8.0.x 10.8.0.1"
  if (m_assigned_ip.empty() && line.find("ifconfig ") != std::string::npos) {
    std::istringstream ss(line);
    std::string tok;
    while (ss >> tok) {
      if (tok.find("ifconfig") != std::string::npos) {
        std::string dev, ip;
        if (ss >> dev >> ip && looks_like_ipv4(ip)) {
          m_assigned_ip = ip;
          goto vip_found;
        }
      }
    }
  }

  // Management/env style: "ifconfig_local=10.8.0.x"
  if (m_assigned_ip.empty() && line.find("ifconfig_local=") != std::string::npos) {
    const auto vip = token_after(line, "ifconfig_local=");
    if (looks_like_ipv4(vip)) { m_assigned_ip = vip; goto vip_found; }
  }

  // Kernel net API (OpenVPN 2.5+ on Linux):
  //   "net_addr_ptp_v4_add: 10.8.0.6 peer 10.8.0.5 dev tun0"
  if (m_assigned_ip.empty() && line.find("net_addr_ptp_v4_add:") != std::string::npos) {
    const auto vip = token_after(line, "net_addr_ptp_v4_add: ");
    if (looks_like_ipv4(vip)) { m_assigned_ip = vip; goto vip_found; }
  }

  return;

vip_found:
  std::cout << "[openvpn_client] detected VIP=" << m_assigned_ip << '\n';
  if (m_tunnel_up) setup_nat(m_assigned_ip);
}

// ---------------------------------------------------------------------------
// NAT forwarding — iptables PREROUTING DNAT
// ---------------------------------------------------------------------------

void openvpn_client::setup_nat(const std::string &vip) {
#ifdef __linux__
  if (vip.empty() || m_fwd_host.empty() || m_fwd_ports.empty()) return;

  std::system("sysctl -w net.ipv4.ip_forward=1 >/dev/null 2>&1");
  for (uint16_t port : m_fwd_ports) {
    const std::string rule =
        "iptables -t nat -C PREROUTING"
        " -d " + vip + " -p tcp --dport " + std::to_string(port) +
        " -j DNAT --to-destination " + m_fwd_host + ":" + std::to_string(port) +
        " 2>/dev/null || "
        "iptables -t nat -A PREROUTING"
        " -d " + vip + " -p tcp --dport " + std::to_string(port) +
        " -j DNAT --to-destination " + m_fwd_host + ":" + std::to_string(port);
    std::system(rule.c_str());
    std::cout << "[openvpn_client] DNAT " << vip << ":" << port
              << " \xe2\x86\x92 " << m_fwd_host << ":" << port << '\n';
  }
  std::system("iptables -t nat -A POSTROUTING -j MASQUERADE 2>/dev/null || true");
#endif
}

void openvpn_client::teardown_nat(const std::string &vip) {
#ifdef __linux__
  if (vip.empty() || m_fwd_host.empty()) return;
  for (uint16_t port : m_fwd_ports) {
    const std::string rule =
        "iptables -t nat -D PREROUTING"
        " -d " + vip + " -p tcp --dport " + std::to_string(port) +
        " -j DNAT --to-destination " + m_fwd_host + ":" + std::to_string(port) +
        " 2>/dev/null || true";
    std::system(rule.c_str());
  }
  std::cout << "[openvpn_client] DNAT rules removed for " << vip << '\n';
#endif
}

#endif // __openvpn_client_cpp__
