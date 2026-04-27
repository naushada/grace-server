#ifndef __main_app_cpp__
#define __main_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"
#include "fs_app.hpp"
#include "gnmi_client.hpp"
#include "openvpn_client.hpp"
#include "openvpn_server.hpp"
#include "server_app.hpp"
#include "tls_config.hpp"

#include "gnmi/gnmi.pb.h"
#include <mosquitto.h>

#include <iomanip>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Argument parsing helpers
// ---------------------------------------------------------------------------

static std::string get_flag(int argc, const char *argv[],
                              const std::string &name,
                              const std::string &def = "") {
  const std::string prefix = "--" + name + "=";
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg.starts_with(prefix))
      return std::string(arg.substr(prefix.size()));
  }
  return def;
}

static uint16_t get_port_flag(int argc, const char *argv[],
                                const std::string &name, uint16_t def) {
  const auto s = get_flag(argc, argv, name, "");
  return s.empty() ? def : static_cast<uint16_t>(std::stoi(s));
}

// ---------------------------------------------------------------------------
// Usage
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// gnmi-mqtt-client helpers
// ---------------------------------------------------------------------------

struct gnmi_pub_ctx {
  struct mosquitto *mosq;
  std::string       topic;
  std::string       payload;
  int               interval_s;
  struct event     *timer;
};

// Timer callback: publishes the gNMI proto to MQTT then re-arms itself.
static void gnmi_pub_cb(evutil_socket_t, short, void *arg) {
  auto *c = static_cast<gnmi_pub_ctx *>(arg);
  mosquitto_loop(c->mosq, 0, 1);
  const int rc = mosquitto_publish(c->mosq, nullptr, c->topic.c_str(),
                                    static_cast<int>(c->payload.size()),
                                    c->payload.data(), 0, false);
  if (rc == MOSQ_ERR_SUCCESS)
    std::cout << "[gnmi-mqtt-client] published " << c->payload.size()
              << "B → topic=" << c->topic << '\n';
  else
    std::cerr << "[gnmi-mqtt-client] publish error: "
              << mosquitto_strerror(rc) << '\n';
  const struct timeval tv{c->interval_s, 0};
  evtimer_add(c->timer, &tv);
}

static void print_usage(const char *prog) {
  std::cerr
    << "Usage:\n"
    << "  " << prog << " --mode=server [options]\n"
    << "       Runs the OpenVPN tunnel server (port 1194) and a gNMI server\n"
    << "       (port 58989).  When --gnmi-push=true, fires a gNMI Get to each\n"
    << "       connecting VPN client's gNMI server after --gnmi-push-delay seconds.\n"
    << "\n"
    << "  " << prog << " --mode=client [options]\n"
    << "       Connects to openvpn_server, receives a virtual IP, configures tun0,\n"
    << "       then starts its own gNMI server on --gnmi-port (default 58989) so\n"
    << "       the VPN server can push gNMI updates back through the tunnel.\n"
    << "\n"
    << "  Client options:\n"
    << "    --server=<host>          VPN server address        (default: 127.0.0.1)\n"
    << "    --port=<port>            VPN server port           (default: 1194)\n"
    << "    --status=<path>          Lua status file           (default: /run/vpn_status.lua)\n"
    << "    --gnmi-port=<port>       gNMI server listen port   (default: 58989)\n"
    << "    --tls=true               Enable TLS on VPN tunnel  (default: false)\n"
    << "    --cert=<path>            PEM certificate\n"
    << "    --key=<path>             PEM private key\n"
    << "    --ca=<path>              PEM CA certificate\n"
    << "    --gnmi-tls=true          Enable TLS on gNMI server (default: false)\n"
    << "    --gnmi-cert=<path>       PEM server cert for gNMI TLS\n"
    << "    --gnmi-key=<path>        PEM private key for gNMI TLS\n"
    << "    --gnmi-ca=<path>         PEM CA cert for gNMI TLS\n"
    << "    --gnmi-probe=true        Also fire a one-shot gNMI Get to the server VIP\n"
    << "    --server-vip=<ip>        Server-side VPN IP to probe  (default: 10.8.0.1)\n"
    << "\n"
    << "  Server options:\n"
    << "    --server-ip=<ip>         Server TUN IP             (default: 10.8.0.1)\n"
    << "    --pool-start=<ip>        First client IP           (default: 10.8.0.2)\n"
    << "    --pool-end=<ip>          Last client IP            (default: 10.8.0.254)\n"
    << "    --netmask=<mask>         Tunnel netmask            (default: 255.255.255.0)\n"
    << "    --tls=true               Enable TLS on VPN tunnel  (default: false)\n"
    << "    --cert/--key/--ca        PEM files for VPN TLS\n"
    << "    --gnmi-tls=true          Enable TLS on gNMI server (default: false)\n"
    << "    --gnmi-cert/--gnmi-key/--gnmi-ca  PEM files for gNMI TLS\n"
    << "    --gnmi-push=true         Push gNMI Get to each client after tunnel up\n"
    << "    --gnmi-port=<port>       Client gNMI port to push to  (default: 58989)\n"
    << "    --gnmi-push-delay=<s>    Seconds to wait before push (default: 2)\n"
    << "\n"
    << "  " << prog << " --mode=gnmi-mqtt-client [options]\n"
    << "       Publishes a gNMI GetRequest protobuf to an MQTT broker.\n"
    << "       topic   = --mqtt-topic (the target client's virtual IP)\n"
    << "       payload = serialised gnmi.GetRequest proto bytes\n"
    << "       The openvpn_server (--mqtt-host) subscribes, forwards the\n"
    << "       request through the VPN tunnel, and the client's nftables\n"
    << "       DNAT rule delivers it to gnmi-server-svc.\n"
    << "\n"
    << "  " << prog << " --mode=gnmi-server [options]\n"
    << "       Standalone gNMI server — no VPN, no TUN device needed.\n"
    << "       Intended for gnmi-server-svc behind an nftables/socat proxy.\n"
    << "    --gnmi-port=<port>       Listen port  (default: 58989)\n"
    << "    --gnmi-tls/--gnmi-cert/--gnmi-key/--gnmi-ca  TLS options\n"
    << "\n"
    << "  gnmi-mqtt-client options:\n"
    << "    --mqtt-host=<host>       MQTT broker address       (default: localhost)\n"
    << "    --mqtt-port=<port>       MQTT broker port          (default: 1883)\n"
    << "    --mqtt-topic=<ip>        Target client virtual IP  (required)\n"
    << "    --interval=<s>           Publish interval seconds  (default: 10)\n";
}

// ---------------------------------------------------------------------------
// gNMI probe — fires after the VPN tunnel is up.
//
// Design: phased event loop (sequential, NOT nested):
//
//   Phase 1  while (!vpn.ip_assigned()) event_base_loop(EVLOOP_ONCE)
//            ↳ drives the openvpn_client bufferevent until IP_ASSIGN arrives
//              and tun0 is configured
//
//   Phase 2  gnmi_client::call(server_vip, gnmi_port, "/gnmi.gNMI/Get", req)
//            ↳ internally loops event_base_loop(EVLOOP_ONCE) until response
//              arrives — safe because Phase 1 has already returned, no nesting
//
//   Phase 3  event_base_dispatch(base)
//            ↳ keeps the tunnel alive; tun I/O and reconnect logic continue
//
// event_base_loop(EVLOOP_ONCE) is NOT re-entrant — calling it from within an
// already-running dispatch would be undefined behaviour.  The phased approach
// avoids that by never calling it from inside an event callback.
// ---------------------------------------------------------------------------

static void dump_gnmi_response(const gnmi_client::response &r) {
  std::cout << "[gnmi-probe] grpc_status=" << r.grpc_status;
  if (!r.grpc_message.empty())
    std::cout << " message=\"" << r.grpc_message << '"';

  if (r.grpc_status == 0 && !r.body_pb.empty()) {
    gnmi::GetResponse resp;
    if (resp.ParseFromString(r.body_pb)) {
      std::cout << " notification_count=" << resp.notification_size();
      for (int i = 0; i < resp.notification_size(); ++i) {
        const auto &n = resp.notification(i);
        std::cout << "\n  [" << i << "] timestamp=" << n.timestamp()
                  << " update_count=" << n.update_size();
      }
    } else {
      // Fallback: hex dump of raw proto bytes
      std::ostringstream hex;
      hex << std::hex << std::setfill('0');
      for (unsigned char c : r.body_pb)
        hex << std::setw(2) << static_cast<int>(c);
      std::cout << " body_hex=" << hex.str();
    }
  }
  std::cout << '\n';
}

// ---------------------------------------------------------------------------
// Entry point
// ---------------------------------------------------------------------------

int main(int argc, const char *argv[]) {
  // Docker pipes stdout through a non-TTY, making it block-buffered by default.
  // Force unit (per-write) flushing so log lines appear immediately.
  std::cout << std::unitbuf;

  const std::string mode = get_flag(argc, argv, "mode", "server");

  // ── gnmi-mqtt-client ─────────────────────────────────────────────────────
  // Minimal flow: connect to MQTT broker, publish gNMI GetRequest repeatedly.
  // Handled before the shared TLS/fs_app setup used by server/client modes.
  if (mode == "gnmi-mqtt-client") {
    const std::string mqtt_host  = get_flag(argc, argv, "mqtt-host", "localhost");
    const uint16_t    mqtt_port  = get_port_flag(argc, argv, "mqtt-port", 1883);
    const std::string mqtt_topic = get_flag(argc, argv, "mqtt-topic", "");
    const int         interval_s = std::stoi(get_flag(argc, argv, "interval", "10"));

    if (mqtt_topic.empty()) {
      std::cerr << "[main] --mqtt-topic is required for gnmi-mqtt-client\n";
      return 1;
    }
    std::cout << "[main] mode=gnmi-mqtt-client mqtt=" << mqtt_host
              << ":" << mqtt_port << " topic=" << mqtt_topic
              << " interval=" << interval_s << "s\n";

    gnmi::GetRequest req;
    req.mutable_prefix()->set_target("VIEWER");
    req.add_path()->add_elem()->set_name("interfaces");
    req.set_encoding(gnmi::JSON);
    std::string req_pb;
    req.SerializeToString(&req_pb);

    mosquitto_lib_init();
    struct mosquitto *mosq = mosquitto_new("gnmi-client-svc", true, nullptr);
    if (!mosq) {
      std::cerr << "[main] mosquitto_new failed\n";
      mosquitto_lib_cleanup();
      return 1;
    }
    const int rc = mosquitto_connect(mosq, mqtt_host.c_str(), mqtt_port, 60);
    if (rc != MOSQ_ERR_SUCCESS) {
      std::cerr << "[main] MQTT connect to " << mqtt_host << ":" << mqtt_port
                << " failed: " << mosquitto_strerror(rc) << '\n';
      mosquitto_destroy(mosq);
      mosquitto_lib_cleanup();
      return 1;
    }

    auto *ctx    = new gnmi_pub_ctx{mosq, mqtt_topic, req_pb, interval_s, nullptr};
    ctx->timer   = evtimer_new(evt_base::instance().get(), gnmi_pub_cb, ctx);
    const struct timeval tv{0, 0}; // fire immediately on first tick
    evtimer_add(ctx->timer, &tv);

    run_evt_loop{}();

    event_free(ctx->timer);
    delete ctx;
    mosquitto_disconnect(mosq);
    mosquitto_destroy(mosq);
    mosquitto_lib_cleanup();
    return 0;
  }

  // ── gnmi-server ──────────────────────────────────────────────────────────
  // Pure gNMI server with no VPN — used by gnmi-server-svc in docker-compose.
  // Listens on --gnmi-port (default 58989); TLS optional via --gnmi-tls flags.
  if (mode == "gnmi-server") {
    const tls_config gnmi_tls{
      get_flag(argc, argv, "gnmi-tls", "false") == "true",
      get_flag(argc, argv, "gnmi-cert", ""),
      get_flag(argc, argv, "gnmi-key",  ""),
      get_flag(argc, argv, "gnmi-ca",   ""),
    };
    const uint16_t gnmi_port = get_port_flag(argc, argv, "gnmi-port", 58989);
    std::cout << "[main] mode=gnmi-server port=" << gnmi_port
              << " tls=" << (gnmi_tls.enabled ? "ON" : "OFF") << '\n';
    server gnmi_svc("0.0.0.0", gnmi_port, gnmi_tls);
    run_evt_loop{}();
    return 0;
  }

  if (mode != "server" && mode != "client") {
    std::cerr << "[main] unknown mode '" << mode << "'\n";
    print_usage(argv[0]);
    return 1;
  }

  // VPN TLS — shared across both modes.
  const tls_config tls{
    get_flag(argc, argv, "tls", "false") == "true",
    get_flag(argc, argv, "cert", ""),
    get_flag(argc, argv, "key",  ""),
    get_flag(argc, argv, "ca",   ""),
  };
  // gNMI TLS — independent from VPN TLS.
  const tls_config gnmi_tls{
    get_flag(argc, argv, "gnmi-tls", "false") == "true",
    get_flag(argc, argv, "gnmi-cert", ""),
    get_flag(argc, argv, "gnmi-key",  ""),
    get_flag(argc, argv, "gnmi-ca",   ""),
  };
  fs_app fs_mon("/app/command");

  if (mode == "client") {
    const std::string vpn_server  = get_flag(argc, argv, "server", "127.0.0.1");
    const uint16_t    port        = get_port_flag(argc, argv, "port", 1194);
    const std::string status_file = get_flag(argc, argv, "status", "/run/vpn_status.lua");
    const bool        gnmi_probe  = get_flag(argc, argv, "gnmi-probe", "false") == "true";
    const std::string server_vip  = get_flag(argc, argv, "server-vip", "10.8.0.1");
    const uint16_t    gnmi_port   = get_port_flag(argc, argv, "gnmi-port", 58989);

    std::cout << "[main] mode=client server=" << vpn_server << " port=" << port
              << " tls=" << (tls.enabled ? "ON" : "OFF")
              << (gnmi_probe ? " gnmi-probe=ON" : "") << '\n';

    // vpn_client + gnmi_svc + event loop must stay in the same scope so their
    // bufferevents are alive for the entire duration of run_evt_loop.
    openvpn_client vpn_client(vpn_server, port, status_file, tls);

    // Phase 1: wait for the VPN tunnel and tun0 to come up before binding the
    // gNMI server — the server will be reachable by openvpn_server via the tunnel.
    std::cout << "[main] waiting for VPN tunnel...\n";
    auto *base = evt_base::instance().get();
    while (!vpn_client.ip_assigned())
      event_base_loop(base, EVLOOP_ONCE);

    std::cout << "[main] tunnel up, assigned=" << vpn_client.assigned_ip()
              << " — starting gNMI server on port " << gnmi_port
              << " tls=" << (gnmi_tls.enabled ? "ON" : "OFF") << '\n';

    // Phase 2: start the gNMI server — now reachable via tun0.
    // openvpn_server connects here (tunnel IP:gnmi_port) after the push delay.
    server gnmi_svc("0.0.0.0", gnmi_port, gnmi_tls);

    // Phase 3 (optional): fire a one-shot gNMI probe to the VPN server.
    if (gnmi_probe) {
      std::cout << "[main] probing " << server_vip << ":" << gnmi_port << "\n";

      gnmi::GetRequest req;
      req.mutable_prefix()->set_target("VIEWER");
      auto *path = req.add_path();
      path->add_elem()->set_name("interfaces");
      req.set_encoding(gnmi::JSON);

      std::string req_pb;
      req.SerializeToString(&req_pb);

      const auto resp = gnmi_client::call(server_vip, gnmi_port,
                                           "/gnmi.gNMI/Get", req_pb, gnmi_tls);
      dump_gnmi_response(resp);
      std::cout << "[main] probe done, tunnel remains open\n";
    }

    run_evt_loop{}();

  } else {
    const std::string pool_start = get_flag(argc, argv, "pool-start", "10.8.0.2");
    const std::string pool_end   = get_flag(argc, argv, "pool-end",   "10.8.0.254");
    const std::string server_ip  = get_flag(argc, argv, "server-ip",  "10.8.0.1");
    const std::string netmask    = get_flag(argc, argv, "netmask",    "255.255.255.0");

    // Build a gNMI GetRequest once; openvpn_server fires it at each connecting client.
    gnmi_push_cfg gnmi_push;
    gnmi_push.enabled  = get_flag(argc, argv, "gnmi-push", "false") == "true";
    gnmi_push.port     = get_port_flag(argc, argv, "gnmi-port", 58989);
    gnmi_push.tls      = gnmi_tls;
    gnmi_push.delay_s  = static_cast<int>(
        std::stoi(get_flag(argc, argv, "gnmi-push-delay", "2")));
    if (gnmi_push.enabled) {
      gnmi::GetRequest req;
      req.mutable_prefix()->set_target("VIEWER");
      req.add_path()->add_elem()->set_name("interfaces");
      req.set_encoding(gnmi::JSON);
      req.SerializeToString(&gnmi_push.request_pb);
    }

    // Optional MQTT subscriber: receives gNMI requests from gnmi-client-svc
    // and routes them into the VPN tunnel via push_async.
    mqtt_sub_cfg mqtt_sub;
    mqtt_sub.enabled   = !get_flag(argc, argv, "mqtt-host", "").empty();
    mqtt_sub.host      = get_flag(argc, argv, "mqtt-host", "localhost");
    mqtt_sub.port      = get_port_flag(argc, argv, "mqtt-port", 1883);
    mqtt_sub.gnmi_port = get_port_flag(argc, argv, "gnmi-port", 58989);

    std::cout << "[main] mode=server tls=" << (tls.enabled ? "ON" : "OFF")
              << " gnmi-tls=" << (gnmi_tls.enabled ? "ON" : "OFF")
              << " gnmi-push=" << (gnmi_push.enabled ? "ON" : "OFF")
              << " mqtt=" << (mqtt_sub.enabled ? "ON" : "OFF")
              << " pool=" << pool_start << "–" << pool_end << '\n';

    // svc_module + vpn must stay in scope for the entire event loop.
    server svc_module("0.0.0.0", 58989, gnmi_tls);
    openvpn_server vpn("0.0.0.0", 1194, pool_start, pool_end, tls,
                       server_ip, netmask, gnmi_push, mqtt_sub);

    run_evt_loop{}();
  }

  return 0;
}

#endif // __main_app_cpp__
