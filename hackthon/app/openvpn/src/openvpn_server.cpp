#ifndef __openvpn_server_cpp__
#define __openvpn_server_cpp__

#include "openvpn_server.hpp"
#include "openvpn_peer.hpp"
#include "gnmi_client.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <iostream>

#ifdef __linux__
#include <fcntl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <linux/sockios.h>
#include <net/route.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

// ---------------------------------------------------------------------------
// ip_pool
// ---------------------------------------------------------------------------

uint32_t ip_pool::to_u32(const std::string &ip) {
  struct in_addr a{};
  inet_pton(AF_INET, ip.c_str(), &a);
  return ntohl(a.s_addr); // host byte order for arithmetic comparison
}

std::string ip_pool::to_str(uint32_t addr) {
  struct in_addr a{};
  a.s_addr = htonl(addr);
  char buf[INET_ADDRSTRLEN];
  inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}

ip_pool::ip_pool(const std::string &start_ip, const std::string &end_ip) {
  const uint32_t lo = to_u32(start_ip);
  const uint32_t hi = to_u32(end_ip);
  for (uint32_t a = lo; a <= hi; ++a)
    m_free.insert(a);
}

std::string ip_pool::assign(int32_t channel) {
  if (m_free.empty()) return {};
  const uint32_t addr = *m_free.begin();
  m_free.erase(m_free.begin());
  m_assigned[channel] = addr;
  return to_str(addr);
}

void ip_pool::release(int32_t channel) {
  auto it = m_assigned.find(channel);
  if (it != m_assigned.end()) {
    m_free.insert(it->second);
    m_assigned.erase(it);
  }
}

std::string ip_pool::get(int32_t channel) const {
  auto it = m_assigned.find(channel);
  return it != m_assigned.end() ? to_str(it->second) : std::string{};
}

// ---------------------------------------------------------------------------
// openvpn_server
// ---------------------------------------------------------------------------

openvpn_server::openvpn_server(const std::string   &host,
                                 uint16_t             port,
                                 const std::string   &pool_start,
                                 const std::string   &pool_end,
                                 const tls_config    &tls,
                                 const std::string   &server_ip,
                                 const std::string   &netmask,
                                 const gnmi_push_cfg &gnmi_push,
                                 const mqtt_sub_cfg  &mqtt_sub)
    : evt_io(host, port, tls.build_server_ctx(), listener_tag{}),
      m_pool(pool_start, pool_end), m_netmask(netmask),
      m_gnmi_push(gnmi_push) {
  std::cout << "[openvpn_server] " << host << ":" << port
            << " pool=" << pool_start << "–" << pool_end
            << " tls=" << (tls.enabled ? "ON" : "OFF")
            << " gnmi-push=" << (gnmi_push.enabled ? "ON" : "OFF")
            << " mqtt=" << (mqtt_sub.enabled ? "ON" : "OFF") << '\n';
  open_server_tun(server_ip);
  if (mqtt_sub.enabled)
    setup_mqtt(mqtt_sub);
}

openvpn_server::~openvpn_server() {
  if (m_mqtt_poll_timer) { event_free(m_mqtt_poll_timer); m_mqtt_poll_timer = nullptr; }
  if (m_mosq) {
    mosquitto_disconnect(m_mosq);
    mosquitto_destroy(m_mosq);
    mosquitto_lib_cleanup();
    m_mosq = nullptr;
  }
  m_server_tun_io.reset();
  if (m_server_tun_fd >= 0) ::close(m_server_tun_fd);
  m_peers.clear();
}

// ---------------------------------------------------------------------------
// MQTT subscriber — receives gNMI requests and forwards into the VPN tunnel
// ---------------------------------------------------------------------------

void openvpn_server::on_mqtt_message(struct mosquitto * /*mosq*/, void *userdata,
                                      const struct mosquitto_message *msg) {
  if (!msg || !msg->payload || msg->payloadlen <= 0) return;
  auto *self    = static_cast<openvpn_server *>(userdata);
  const std::string topic(msg->topic);
  const std::string payload(static_cast<const char *>(msg->payload),
                             static_cast<std::size_t>(msg->payloadlen));
  std::cout << "[openvpn_server] MQTT ← topic=" << topic
            << " payload=" << payload.size() << "B"
            << " → push_async to " << topic << ":" << self->m_mqtt_gnmi_port << '\n';
  gnmi_client::push_async(topic, self->m_mqtt_gnmi_port,
                           "/gnmi.gNMI/Get", payload, {});
}

void openvpn_server::mqtt_poll_cb(evutil_socket_t, short, void *arg) {
  auto *self = static_cast<openvpn_server *>(arg);
  mosquitto_loop(self->m_mosq, 0, 1);
  const struct timeval tv{0, 100'000}; // re-arm every 100 ms
  evtimer_add(self->m_mqtt_poll_timer, &tv);
}

void openvpn_server::setup_mqtt(const mqtt_sub_cfg &cfg) {
  mosquitto_lib_init();
  m_mqtt_gnmi_port = cfg.gnmi_port;
  m_mosq = mosquitto_new("vpn-server", true, this);
  if (!m_mosq) {
    std::cerr << "[openvpn_server] mosquitto_new failed\n";
    return;
  }
  mosquitto_message_callback_set(m_mosq, on_mqtt_message);
  const int rc = mosquitto_connect(m_mosq, cfg.host.c_str(), cfg.port, 60);
  if (rc != MOSQ_ERR_SUCCESS) {
    std::cerr << "[openvpn_server] MQTT connect to " << cfg.host << ":" << cfg.port
              << " failed: " << mosquitto_strerror(rc) << '\n';
    mosquitto_destroy(m_mosq);
    m_mosq = nullptr;
    return;
  }
  mosquitto_subscribe(m_mosq, nullptr, "#", 0);
  std::cout << "[openvpn_server] MQTT subscribed to # on "
            << cfg.host << ":" << cfg.port << '\n';
  m_mqtt_poll_timer = evtimer_new(evt_base::instance().get(), mqtt_poll_cb, this);
  const struct timeval tv{0, 100'000};
  evtimer_add(m_mqtt_poll_timer, &tv);
}

// Context block passed to gnmi_push_cb via evtimer_new void* arg.
struct gnmi_push_ctx {
  openvpn_server *server;
  std::string     client_ip;
  struct event   *timer{nullptr};
};

void openvpn_server::gnmi_push_cb(evutil_socket_t, short, void *arg) {
  auto *ctx = static_cast<gnmi_push_ctx *>(arg);
  std::cout << "[openvpn_server] gNMI push → "
            << ctx->client_ip << ":" << ctx->server->m_gnmi_push.port << '\n';
  gnmi_client::push_async(ctx->client_ip,
                           ctx->server->m_gnmi_push.port,
                           ctx->server->m_gnmi_push.rpc_path,
                           ctx->server->m_gnmi_push.request_pb,
                           ctx->server->m_gnmi_push.tls);
  event_free(ctx->timer);
  delete ctx;
}

std::int32_t openvpn_server::handle_connect(const handle_t &channel,
                                              const std::string &peer_host) {
  const std::string ip = m_pool.assign(channel);
  if (ip.empty()) {
    std::cerr << "[openvpn_server] pool exhausted, rejecting " << peer_host << '\n';
    return -1;
  }

  // wrap_accepted() is in evt_io: returns a TLS bev when the server was
  // constructed with a TLS ctx, plain socket bev otherwise.
  auto *bev = wrap_accepted(channel);
  auto peer = std::make_unique<openvpn_peer>(bev, peer_host, this, ip, m_netmask);
  m_peers.emplace(channel, std::move(peer));
  manage_client_route(ip, true);
  std::cout << "[openvpn_server] accepted " << peer_host
            << " \xe2\x86\x92 " << ip << (has_tls() ? " (TLS)" : "") << '\n';

  // Schedule a gNMI push to the newly-connected client after a short delay
  // so the client has time to configure its tun0 and start its gNMI server.
  if (m_gnmi_push.enabled && !m_gnmi_push.request_pb.empty()) {
    auto *ctx   = new gnmi_push_ctx{this, ip};
    ctx->timer  = evtimer_new(evt_base::instance().get(), gnmi_push_cb, ctx);
    const struct timeval tv{static_cast<time_t>(m_gnmi_push.delay_s), 0};
    evtimer_add(ctx->timer, &tv);
    std::cout << "[openvpn_server] gNMI push to " << ip
              << " scheduled in " << m_gnmi_push.delay_s << "s\n";
  }

  return 0;
}

std::int32_t openvpn_server::handle_close(const handle_t &channel) {
  const std::string ip = m_pool.get(channel);
  if (!ip.empty()) manage_client_route(ip, false);
  m_pool.release(channel);
  m_peers.erase(channel);
  return 0;
}

// ---------------------------------------------------------------------------
// ip_pool — reverse lookup: IP string → channel
// ---------------------------------------------------------------------------

int32_t ip_pool::find_channel(const std::string &ip) const {
  const uint32_t addr = to_u32(ip);
  for (const auto &[ch, a] : m_assigned)
    if (a == addr) return ch;
  return -1;
}

// ---------------------------------------------------------------------------
// Server TUN — receives IP packets from local stack, routes to right peer
// ---------------------------------------------------------------------------

// server_tun_io — wraps server TUN fd in evt_io; routes inbound IP packets to peers.
class openvpn_server::server_tun_io : public evt_io {
public:
  server_tun_io(evutil_socket_t fd, openvpn_server &owner)
      : evt_io(fd, "tun"), m_owner(owner) {}

  std::int32_t handle_read(const std::int32_t &, const std::string &data,
                            const bool &dry_run) override {
    if (dry_run || data.size() < 20) return 0;
    char dst[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, data.data() + 16, dst, sizeof(dst));
    const int32_t ch = m_owner.m_pool.find_channel(dst);
    if (ch >= 0) {
      auto it = m_owner.m_peers.find(ch);
      if (it != m_owner.m_peers.end())
        it->second->forward_data(data);
    }
    return 0;
  }
private:
  openvpn_server &m_owner;
};

int openvpn_server::open_server_tun(const std::string &server_ip) {
#ifdef __linux__
  m_server_tun_fd = ::open("/dev/net/tun", O_RDWR);
  if (m_server_tun_fd < 0) {
    std::cerr << "[openvpn_server] open tun: " << strerror(errno) << '\n';
    return -1;
  }
  struct ifreq ifr{};
  ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
  if (::ioctl(m_server_tun_fd, TUNSETIFF, &ifr) < 0) {
    std::cerr << "[openvpn_server] TUNSETIFF: " << strerror(errno) << '\n';
    ::close(m_server_tun_fd); m_server_tun_fd = -1; return -1;
  }
  m_server_tun_name = ifr.ifr_name;
  // Assign server IP and bring up
  int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  auto *sin = reinterpret_cast<struct sockaddr_in *>(&ifr.ifr_addr);
  sin->sin_family = AF_INET;
  inet_pton(AF_INET, server_ip.c_str(), &sin->sin_addr);
  ::ioctl(sock, SIOCSIFADDR, &ifr);
  inet_pton(AF_INET, m_netmask.c_str(), &sin->sin_addr);
  ::ioctl(sock, SIOCSIFNETMASK, &ifr);
  ::ioctl(sock, SIOCGIFFLAGS, &ifr);
  ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
  ::ioctl(sock, SIOCSIFFLAGS, &ifr);
  ::close(sock);
  std::cout << "[openvpn_server] tun " << ifr.ifr_name
            << " configured: " << server_ip << "/24 UP\n";
  m_server_tun_io = std::make_unique<server_tun_io>(m_server_tun_fd, *this);
#endif
  return 0;
}

void openvpn_server::manage_client_route(const std::string &client_ip, bool add) {
#ifdef __linux__
  if (m_server_tun_name.empty()) return;
  struct rtentry rt{};
  auto set_addr = [](struct sockaddr &sa, const std::string &ip) {
    auto *s = reinterpret_cast<struct sockaddr_in *>(&sa);
    s->sin_family = AF_INET;
    inet_pton(AF_INET, ip.c_str(), &s->sin_addr);
  };
  set_addr(rt.rt_dst,     client_ip);
  set_addr(rt.rt_genmask, "255.255.255.255"); // host route
  rt.rt_flags = RTF_UP | RTF_HOST;
  rt.rt_dev   = const_cast<char *>(m_server_tun_name.c_str());
  int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (::ioctl(sock, add ? SIOCADDRT : SIOCDELRT, &rt) < 0)
    std::cerr << "[openvpn_server] route " << (add ? "add" : "del")
              << " " << client_ip << ": " << strerror(errno) << '\n';
  else
    std::cout << "[openvpn_server] route " << (add ? "added" : "removed")
              << " host " << client_ip << " dev " << m_server_tun_name << '\n';
  ::close(sock);
#endif
}

#endif // __openvpn_server_cpp__
