#ifndef __openvpn_server_cpp__
#define __openvpn_server_cpp__

#include "openvpn_server.hpp"
#include "gnmi_client.hpp"

#include <cstring>
#include <iostream>
#include <sstream>

#ifdef __linux__
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// proc_io — libevent reader for the pipe connected to openvpn server stdout
// ---------------------------------------------------------------------------

class openvpn_server::proc_io : public evt_io {
public:
  proc_io(evutil_socket_t fd, openvpn_server & /*owner*/)
      : evt_io(fd, "ovpn-server") {}

  std::int32_t handle_read(const std::int32_t &, const std::string &data,
                            const bool &dry_run) override {
    if (dry_run) return 0;
    // Stream openvpn log to our stdout for visibility.
    for (char c : data)
      if (c != '\r') std::cout.put(c);
    return 0;
  }
};

// ---------------------------------------------------------------------------
// mgmt_io — TCP client connected to openvpn's management interface
// ---------------------------------------------------------------------------

class openvpn_server::mgmt_io : public evt_io {
public:
  static constexpr int TIMER_POLL    = 0;  // repeating status-poll timer
  static constexpr int TIMER_CONNECT = 1;  // initial connect-retry timer

  mgmt_io(uint16_t mgmt_port, openvpn_server &owner)
      : evt_io("127.0.0.1", mgmt_port, false),
        m_mgmt_port(mgmt_port), m_owner(owner) {}

  // Connection established: start the status-poll loop.
  std::int32_t handle_connect(const std::int32_t &, const std::string &) override {
    std::cout << "[openvpn_server] management interface connected\n";
    // Enable log-level notifications and start 5-second status polling.
    const std::string init = "log on\n";
    tx(init.data(), init.size());
    arm_timer(TIMER_POLL, {5, 0}, /*repeat=*/true);
    return 0;
  }

  // Data from management interface.
  std::int32_t handle_read(const std::int32_t &, const std::string &data,
                            const bool &dry_run) override {
    if (dry_run) return 0;
    m_owner.m_mgmt_buf.append(data);
    size_t pos;
    while ((pos = m_owner.m_mgmt_buf.find('\n')) != std::string::npos) {
      std::string line = m_owner.m_mgmt_buf.substr(0, pos);
      m_owner.m_mgmt_buf.erase(0, pos + 1);
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (!line.empty()) parse_mgmt_line(line);
    }
    return 0;
  }

  // Connection lost or timed-out: retry after 2 seconds.
  std::int32_t handle_event(const std::int32_t &, const std::uint16_t &) override {
    std::cerr << "[openvpn_server] management connection lost, retrying...\n";
    disarm_timer(TIMER_POLL);
    arm_timer(TIMER_CONNECT, {2, 0});
    return 0;
  }

  std::int32_t handle_close(const std::int32_t &) override {
    std::cerr << "[openvpn_server] management interface closed, retrying...\n";
    disarm_timer(TIMER_POLL);
    arm_timer(TIMER_CONNECT, {2, 0});
    return 0;
  }

  std::int32_t handle_write(const std::int32_t &) override { return 0; }

  std::int32_t handle_timeout(int timer_id) override {
    if (timer_id == TIMER_POLL) {
      // Poll for current client list.
      const std::string cmd = "status 2\n";
      tx(cmd.data(), cmd.size());
      return 0;
    }
    // TIMER_CONNECT: reconnect attempt.
    bufferevent_socket_connect_hostname(get_bufferevt(), nullptr,
                                        AF_INET, "127.0.0.1", m_mgmt_port);
    return 0;
  }

private:
  // Parse one line from the management interface.
  // We watch for the ROUTING TABLE section of "status 2" output to extract
  // currently connected client VIPs, then diff against m_owner.m_active_vips.
  void parse_mgmt_line(const std::string &line) {
    // Log lines from openvpn (forwarded via "log on").
    if (line.rfind(">LOG:", 0) == 0) {
      std::cout << "[ovpn-mgmt] " << line << '\n';
      return;
    }

    // --- status 2 output parsing ---

    if (line == "ROUTING TABLE") {
      m_owner.m_in_routing_table = true;
      m_polled_vips.clear();
      return;
    }
    if (line == "GLOBAL STATS" || line == "END") {
      if (m_owner.m_in_routing_table) {
        m_owner.m_in_routing_table = false;
        // Diff: new VIPs → connect, gone VIPs → disconnect
        for (const auto &vip : m_polled_vips) {
          if (!m_owner.m_active_vips.count(vip))
            m_owner.on_client_connect(vip);
        }
        for (const auto &vip : m_owner.m_active_vips) {
          if (!m_polled_vips.count(vip))
            m_owner.on_client_disconnect(vip);
        }
        m_owner.m_active_vips = m_polled_vips;
      }
      return;
    }

    // Routing table row format: "Virtual Address,Common Name,Real Address,Last Ref"
    if (m_owner.m_in_routing_table && !line.empty() && line[0] != 'V') {
      const auto comma = line.find(',');
      if (comma != std::string::npos) {
        const std::string vip = line.substr(0, comma);
        // Skip header row and non-IP lines
        if (vip.find('.') != std::string::npos)
          m_polled_vips.insert(vip);
      }
    }
  }

  uint16_t                        m_mgmt_port;
  openvpn_server                 &m_owner;
  std::unordered_set<std::string> m_polled_vips; // VIPs seen in current status 2 response
};

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

openvpn_server::openvpn_server(uint16_t vpn_port,
                                 const tls_config &tls,
                                 uint16_t mgmt_port,
                                 const mqtt_sub_cfg &mqtt)
    : m_mgmt_port(mgmt_port), m_mqtt(mqtt) {
#ifdef __linux__
  int pipefd[2];
  if (::pipe(pipefd) < 0) {
    std::cerr << "[openvpn_server] pipe: " << strerror(errno) << '\n';
    return;
  }

  std::vector<std::string> args_s = {
    "openvpn",
    "--mode", "server",
    "--dev", "tun",
    "--proto", "tcp-server",
    "--port", std::to_string(vpn_port),
    "--server", "10.8.0.0", "255.255.255.0",
    "--keepalive", "10", "120",
    "--persist-key",
    "--persist-tun",
    "--management", "127.0.0.1", std::to_string(mgmt_port),
    "--verb", "3",
    "--log", "/dev/stdout",
  };
  if (tls.enabled) {
    if (!tls.ca_file.empty())   { args_s.push_back("--ca");   args_s.push_back(tls.ca_file); }
    if (!tls.cert_file.empty()) { args_s.push_back("--cert"); args_s.push_back(tls.cert_file); }
    if (!tls.key_file.empty())  { args_s.push_back("--key");  args_s.push_back(tls.key_file); }
    args_s.push_back("--dh");
    args_s.push_back("/app/certs/dh.pem");
  } else {
    // Control channel TLS still requires cert/key/ca — use baked-in test certs.
    // Data channel: --data-ciphers none + --auth none → completely unencrypted.
    // --dh none uses ECDH so no DH parameter file is needed (OpenVPN 2.4+).
    args_s.insert(args_s.end(), {
      "--ca",           "/app/certs/ca.pem",
      "--cert",         "/app/certs/server.pem",
      "--key",          "/app/certs/server.key",
      "--dh",           "none",
      "--data-ciphers", "none",
      "--auth",         "none",
    });
  }

  std::vector<const char *> argv;
  for (const auto &s : args_s) argv.push_back(s.c_str());
  argv.push_back(nullptr);

  std::cout << "[openvpn_server] spawning: openvpn --server port=" << vpn_port
            << " tls=" << (tls.enabled ? "ON" : "OFF")
            << " mgmt=127.0.0.1:" << mgmt_port
            << " mqtt=" << (mqtt.enabled ? "ON" : "OFF") << '\n';

  m_pid = ::fork();
  if (m_pid < 0) {
    std::cerr << "[openvpn_server] fork: " << strerror(errno) << '\n';
    ::close(pipefd[0]); ::close(pipefd[1]);
    return;
  }
  if (m_pid == 0) {
    ::dup2(pipefd[1], STDOUT_FILENO);
    ::dup2(pipefd[1], STDERR_FILENO);
    ::close(pipefd[0]);
    ::close(pipefd[1]);
    ::execvp("openvpn", const_cast<char *const *>(argv.data()));
    ::perror("execvp openvpn");
    ::_exit(1);
  }

  ::close(pipefd[1]);
  m_pipe_r  = pipefd[0];
  m_proc_io = std::make_unique<proc_io>(m_pipe_r, *this);

  // mgmt_io connects to the management socket immediately; its handle_event
  // will retry every 2s until openvpn has opened the port.
  m_mgmt_io = std::make_unique<mgmt_io>(mgmt_port, *this);
#else
  (void)vpn_port; (void)tls; (void)mgmt_port; (void)mqtt;
  std::cerr << "[openvpn_server] only supported on Linux\n";
#endif
}

openvpn_server::~openvpn_server() {
#ifdef __linux__
  m_mgmt_io.reset();
  m_proc_io.reset();
  m_peers.clear();
  if (m_pipe_r >= 0) { ::close(m_pipe_r); m_pipe_r = -1; }
  if (m_pid > 0) {
    ::kill(m_pid, SIGTERM);
    ::waitpid(m_pid, nullptr, WNOHANG);
    m_pid = -1;
  }
#endif
}

// ---------------------------------------------------------------------------
// Client lifecycle
// ---------------------------------------------------------------------------

void openvpn_server::on_client_connect(const std::string &vip) {
  if (!m_mqtt.enabled || m_peers.count(vip)) return;
  const std::string client_id = "ovpn-srv-peer-" + vip;
  auto mio = std::make_unique<mqtt_io>(
      m_mqtt.host, m_mqtt.port, client_id, on_mqtt_message, this);
  mio->subscribe("fwd/" + vip);
  m_peers.emplace(vip, std::move(mio));
  std::cout << "[openvpn_server] client connected vip=" << vip
            << " — subscribed to fwd/" << vip << '\n';
}

void openvpn_server::on_client_disconnect(const std::string &vip) {
  m_peers.erase(vip);
  std::cout << "[openvpn_server] client disconnected vip=" << vip << '\n';
}

// ---------------------------------------------------------------------------
// MQTT → gNMI forwarding (identical pattern to vpn_peer)
// ---------------------------------------------------------------------------

void openvpn_server::on_mqtt_message(struct mosquitto * /*mosq*/, void *userdata,
                                      const struct mosquitto_message *msg) {
  if (!msg || !msg->payload || msg->payloadlen <= 0) return;
  const std::string topic(msg->topic);
  if (topic.size() <= 4) return;
  const std::string vip = topic.substr(4); // strip "fwd/"

  auto *self = static_cast<openvpn_server *>(userdata);
  auto it = self->m_peers.find(vip);
  if (it == self->m_peers.end()) return;

  const char  *raw = static_cast<const char *>(msg->payload);
  const size_t sz  = static_cast<size_t>(msg->payloadlen);
  const char  *sep = static_cast<const char *>(std::memchr(raw, '\0', sz));
  if (!sep) return;

  const std::string rpc_path(raw, sep - raw);
  const std::string proto_bytes(sep + 1, sz - (sep - raw) - 1);

  std::cout << "[openvpn_server] MQTT \xe2\x86\x90 fwd/" << vip
            << " rpc=" << rpc_path << " " << proto_bytes.size() << "B"
            << " \xe2\x86\x92 gNMI " << vip << ":" << self->m_mqtt.gnmi_port << '\n';

  gnmi_client::push_async(
      vip, self->m_mqtt.gnmi_port, rpc_path, proto_bytes, {},
      [self, vip, rpc_path](const gnmi_client::response &r) {
        auto jt = self->m_peers.find(vip);
        if (jt == self->m_peers.end()) return;
        std::string payload;
        payload += rpc_path; payload += '\0';
        payload += std::to_string(r.grpc_status); payload += '\0';
        payload += r.grpc_message; payload += '\0';
        payload += r.body_pb;
        jt->second->publish("resp/" + vip, payload.data(),
                            static_cast<int>(payload.size()));
        std::cout << "[openvpn_server] MQTT \xe2\x86\x92 resp/" << vip
                  << " status=" << r.grpc_status << '\n';
      });
}

#endif // __openvpn_server_cpp__
