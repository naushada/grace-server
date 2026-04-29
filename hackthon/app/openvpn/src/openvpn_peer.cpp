#ifndef __openvpn_peer_cpp__
#define __openvpn_peer_cpp__

#include "openvpn_peer.hpp"
#include "openvpn_server.hpp"
#include "gnmi_client.hpp"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <openssl/ssl.h>
#include <openssl/x509.h>

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

// Shared post-construction setup called from both constructors.
void openvpn_peer::setup_mqtt(const mqtt_sub_cfg &cfg) {
  if (!cfg.enabled) return;
  m_gnmi_port = cfg.gnmi_port;
  const std::string client_id = "vpn-peer-" + m_assigned_ip;
  m_mqtt_io = std::make_unique<mqtt_io>(
      cfg.host, cfg.port, client_id, on_mqtt_message, this);
  m_mqtt_io->subscribe("fwd/" + m_assigned_ip);
  std::cout << "[openvpn_peer] " << m_assigned_ip
            << " MQTT subscribed to fwd/" << m_assigned_ip
            << " on " << cfg.host << ":" << cfg.port << '\n';
}

openvpn_peer::openvpn_peer(int32_t channel, const std::string &peer_host,
                             openvpn_server *parent,
                             const std::string &assigned_ip,
                             const std::string &netmask,
                             const mqtt_sub_cfg &mqtt)
    : evt_io(channel, peer_host),
      m_parent(parent), m_assigned_ip(assigned_ip), m_netmask(netmask) {
  send_ip_assign();
  setup_mqtt(mqtt);
}

openvpn_peer::openvpn_peer(struct bufferevent *bev, const std::string &peer_host,
                             openvpn_server *parent,
                             const std::string &assigned_ip,
                             const std::string &netmask,
                             const mqtt_sub_cfg &mqtt)
    : evt_io(bev, peer_host),
      m_parent(parent), m_assigned_ip(assigned_ip), m_netmask(netmask) {
  send_ip_assign();
  setup_mqtt(mqtt);
}

// ---------------------------------------------------------------------------
// MQTT — per-peer gNMI forwarding
// ---------------------------------------------------------------------------

// Called when a message arrives on "fwd/<vip>".
// Payload format: rpc_path '\0' proto_bytes
// Forwards to the client's gNMI server through the VPN tunnel, then publishes
// the response on "resp/<vip>" for gnmi-client-svc to relay back to the CLI.
void openvpn_peer::on_mqtt_message(struct mosquitto * /*mosq*/, void *userdata,
                                    const struct mosquitto_message *msg) {
  if (!msg || !msg->payload || msg->payloadlen <= 0) return;
  auto *self = static_cast<openvpn_peer *>(userdata);

  const char  *raw = static_cast<const char *>(msg->payload);
  const size_t sz  = static_cast<size_t>(msg->payloadlen);
  const char  *sep = static_cast<const char *>(std::memchr(raw, '\0', sz));
  if (!sep) return;

  const std::string rpc_path(raw, sep - raw);
  const std::string proto_bytes(sep + 1, sz - (sep - raw) - 1);

  std::cout << "[openvpn_peer] MQTT \xe2\x86\x90 fwd/" << self->m_assigned_ip
            << " rpc=" << rpc_path << " " << proto_bytes.size() << "B"
            << " \xe2\x86\x92 gNMI " << self->m_assigned_ip
            << ":" << self->m_gnmi_port << '\n';

  gnmi_client::push_async(
      self->m_assigned_ip, self->m_gnmi_port, rpc_path, proto_bytes, {},
      [self, rpc_path](const gnmi_client::response &r) {
        if (!self->m_mqtt_io) return;
        // Response payload: rpc_path '\0' grpc_status '\0' grpc_message '\0' body_pb
        std::string payload;
        payload += rpc_path;
        payload += '\0';
        payload += std::to_string(r.grpc_status);
        payload += '\0';
        payload += r.grpc_message;
        payload += '\0';
        payload += r.body_pb;
        const std::string resp_topic = "resp/" + self->m_assigned_ip;
        self->m_mqtt_io->publish(resp_topic, payload.data(),
                                  static_cast<int>(payload.size()));
        std::cout << "[openvpn_peer] MQTT \xe2\x86\x92 " << resp_topic
                  << " status=" << r.grpc_status << '\n';
      });
}

// ---------------------------------------------------------------------------
// TLS handshake complete
// ---------------------------------------------------------------------------

std::string openvpn_peer::extract_cn(struct bufferevent *bev) {
  SSL *ssl = bufferevent_openssl_get_ssl(bev);
  if (!ssl) return {};
  X509 *cert = SSL_get_peer_certificate(ssl);
  if (!cert) return {};
  char cn[256]{};
  X509_NAME_get_text_by_NID(X509_get_subject_name(cert), NID_commonName,
                              cn, sizeof(cn));
  X509_free(cert);
  return cn;
}

std::int32_t openvpn_peer::handle_connect(const std::int32_t &,
                                           const std::string &peer_host) {
  m_peer_cn = extract_cn(get_bufferevt());
  if (m_peer_cn.empty())
    std::cout << "[openvpn_peer] " << peer_host << " connected (plain TCP)\n";
  else
    std::cout << "[openvpn_peer] " << peer_host
              << " authenticated CN=\"" << m_peer_cn << "\"\n";
  return 0;
}

// ---------------------------------------------------------------------------
// Frame helpers
// ---------------------------------------------------------------------------

void openvpn_peer::send_frame(uint8_t type, const std::string &payload) {
  const uint32_t len    = static_cast<uint32_t>(payload.size());
  const uint32_t len_be = htonl(len);
  std::string frame;
  frame.reserve(HEADER_LEN + len);
  frame.push_back(static_cast<char>(type));
  frame.append(reinterpret_cast<const char *>(&len_be), 4);
  frame.append(payload);
  tx(frame.data(), frame.size());
}

void openvpn_peer::send_ip_assign() {
  const std::string payload = m_assigned_ip + " " + m_netmask;
  send_frame(TYPE_IP_ASSIGN, payload);
  std::cout << "[openvpn_peer] IP_ASSIGN " << payload << "\n";
}

size_t openvpn_peer::process_frames(const std::string &buf) {
  size_t consumed = 0;
  while (buf.size() - consumed >= HEADER_LEN) {
    const uint8_t type = static_cast<uint8_t>(buf[consumed]);
    uint32_t len_be = 0;
    std::memcpy(&len_be, buf.data() + consumed + 1, 4);
    const uint32_t len = ntohl(len_be);
    if (buf.size() - consumed < HEADER_LEN + len) break;
    const std::string payload = buf.substr(consumed + HEADER_LEN, len);
    consumed += HEADER_LEN + len;
    switch (type) {
    case TYPE_DATA:
      std::cout << "[openvpn_peer] DATA " << len
                << " bytes from " << m_assigned_ip << "\n";
      break;
    case TYPE_DISCONNECT:
      std::cout << "[openvpn_peer] DISCONNECT from " << m_assigned_ip << "\n";
      break;
    default:
      std::cerr << "[openvpn_peer] unknown frame type 0x"
                << std::hex << int(type) << std::dec << "\n";
      break;
    }
  }
  return consumed;
}

// ---------------------------------------------------------------------------
// evt_io hook overrides
// ---------------------------------------------------------------------------

std::int32_t openvpn_peer::handle_read(const std::int32_t & /*channel*/,
                                        const std::string &data,
                                        const bool &dry_run) {
  if (dry_run) return 0;
  m_recv_buf.append(data);
  const size_t consumed = process_frames(m_recv_buf);
  m_recv_buf.erase(0, consumed);
  return static_cast<int32_t>(consumed);
}

std::int32_t openvpn_peer::handle_close(const std::int32_t &channel) {
  std::cout << "[openvpn_peer] closed, releasing " << m_assigned_ip << "\n";
  // Reset MQTT before calling parent (which destroys this object).
  m_mqtt_io.reset();
  m_parent->handle_close(channel);
  return 0;
}

std::int32_t openvpn_peer::handle_event(const std::int32_t &channel,
                                         const std::uint16_t & /*event*/) {
  std::cerr << "[openvpn_peer] timeout channel=" << channel
            << " ip=" << m_assigned_ip << "\n";
  m_mqtt_io.reset();
  m_parent->handle_close(channel);
  return 0;
}

std::int32_t openvpn_peer::handle_write(const std::int32_t & /*channel*/) {
  return 0;
}

#endif // __openvpn_peer_cpp__
