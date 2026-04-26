# OpenVPN Tunnel — Running in Docker & Packet Flow

## 1. Why TUN inside Docker needs special privileges

TUN interfaces are kernel virtual network devices. Creating one requires:

- `CAP_NET_ADMIN` — to call `TUNSETIFF`, `SIOCSIFADDR`, `SIOCSIFNETMASK`, `SIOCSIFFLAGS`, `SIOCADDRT`
- `/dev/net/tun` device — must be accessible inside the container

Without these the `open("/dev/net/tun")` or ioctl calls fail and `m_tun_fd` stays `-1`.

### Common error: `TUNSETIFF: Operation not permitted`

```
[openvpn_server] TUNSETIFF: Operation not permitted
```

**Cause:** The container image runs as a non-root user (`edge`). Docker adds
`CAP_NET_ADMIN` to the container's *permitted* capability set, but non-root
processes only inherit capabilities into their *effective* set when ambient
capabilities are supported (kernel ≥ 4.3 + runc ≥ 1.0). On many production
hosts this inheritance does not happen, so the `TUNSETIFF` ioctl sees EPERM.

**Fix:** Run the VPN services as `root`. In `docker-compose.yml` the
`x-tun-caps` anchor includes `user: root`, which overrides the `USER edge`
set in the Dockerfile for these services only.

```yaml
x-tun-caps: &tun-caps
  user: root          # ← required for TUNSETIFF
  cap_add:
    - NET_ADMIN
  devices:
    - /dev/net/tun:/dev/net/tun
```

---

## 2. Building the image

```bash
docker build -t grace-server .
```

---

## 3. Running the server container

```bash
docker run -d \
  --name vpn-server \
  --cap-add NET_ADMIN \
  --device /dev/net/tun \
  -p 1194:1194 \
  -p 58989:58989 \
  grace-server \
  /app/app \
    --mode=server \
    --server-ip=10.8.0.1 \
    --pool-start=10.8.0.2 \
    --pool-end=10.8.0.254 \
    --netmask=255.255.255.0
```

What happens inside the container on startup:

```
open("/dev/net/tun")  → fd
TUNSETIFF(IFF_TUN|IFF_NO_PI, name="")  → kernel assigns tun0
SIOCSIFADDR   tun0 = 10.8.0.1
SIOCSIFNETMASK tun0 = 255.255.255.0
SIOCSIFFLAGS  tun0 IFF_UP|IFF_RUNNING
evconnlistener on 0.0.0.0:1194
```

Kernel routing table inside the server container after startup:

```
10.8.0.0/24  dev tun0   (added implicitly by subnet config)
```

---

## 4. Running the client container

```bash
docker run -d \
  --name vpn-client \
  --cap-add NET_ADMIN \
  --device /dev/net/tun \
  grace-server \
  /app/app \
    --mode=client \
    --server=<server-container-ip> \
    --port=1194 \
    --status=/run/vpn_status.lua
```

Get the server container IP if on the same Docker network:

```bash
docker inspect -f '{{range.NetworkSettings.Networks}}{{.IPAddress}}{{end}}' vpn-server
```

Or put both on a named network:

```bash
docker network create vpn-net
docker run -d --name vpn-server --network vpn-net --cap-add NET_ADMIN --device /dev/net/tun ... grace-server /app/app --mode=server
docker run -d --name vpn-client --network vpn-net --cap-add NET_ADMIN --device /dev/net/tun ... grace-server /app/app --mode=client --server=vpn-server ...
```

What happens inside the client container on connect:

```
TCP connect → server:1194
← IP_ASSIGN frame: "10.8.0.3 255.255.255.0"
open("/dev/net/tun") → fd
TUNSETIFF(IFF_TUN|IFF_NO_PI, name="")  → kernel assigns tun0
SIOCSIFADDR    tun0 = 10.8.0.3
SIOCSIFNETMASK tun0 = 255.255.255.0
SIOCSIFFLAGS   tun0 IFF_UP|IFF_RUNNING
evt_io(tun0_fd) registered → tun_io reads outbound packets
```

---

## 5. TLS (optional)

Generate self-signed certs:

```bash
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes
```

Server:

```bash
docker run ... --cap-add NET_ADMIN --device /dev/net/tun \
  -v $(pwd)/certs:/certs \
  grace-server /app/app --mode=server \
    --tls=true --cert=/certs/cert.pem --key=/certs/key.pem
```

Client:

```bash
docker run ... --cap-add NET_ADMIN --device /dev/net/tun \
  -v $(pwd)/certs:/certs \
  grace-server /app/app --mode=client \
    --server=vpn-server --tls=true --ca=/certs/cert.pem
```

---

## 6. Full packet flow — client application → server gNMI service

```
CLIENT CONTAINER                          SERVER CONTAINER
─────────────────────────────────         ─────────────────────────────────

 Application (e.g. gNMI client)
   sends to 10.8.0.1:58989
         │
         ▼
 Kernel IP stack
   route: 10.8.0.0/24 dev tun0
         │
         ▼
 tun0 fd (client-side)
   tun_io::handle_read() fires
         │
         ▼
 openvpn_client::send_frame()
   wraps raw IP packet as:
   [0x02][length 4B BE][IP packet]
         │
         ▼
 TCP tunnel (Docker network / eth0)
   bufferevent → server:1194
         │
         ─────────────────────────────────►
                                          TCP connection (openvpn_peer)
                                          openvpn_peer::handle_read()
                                          process_frames() → TYPE_DATA
                                                │
                                                ▼
                                          openvpn_peer::forward_data()
                                          → NOT used here (this is inbound)
                                          write(server_tun_fd, raw_IP_pkt)
                                                │
                                                ▼
                                          Kernel IP stack (server)
                                          dst=10.8.0.1 → local delivery
                                                │
                                                ▼
                                          gNMI service on 10.8.0.1:58989
                                          processes request, sends reply
                                                │
                                                ▼
                                          Kernel routes reply to 10.8.0.3
                                          via tun0 (10.8.0.0/24 route)
                                                │
                                                ▼
                                          server_tun_io::handle_read()
                                          dst = 10.8.0.3 (bytes 16-19)
                                          ip_pool::find_channel("10.8.0.3")
                                          openvpn_peer::forward_data()
                                          wraps as [0x02][len][IP packet]
         ◄────────────────────────────────
 TCP tunnel (reply frame)
         │
         ▼
 openvpn_client::handle_read()
 process_frames() → TYPE_DATA
         │
         ▼
 write(tun0_fd, raw_IP_pkt)
         │
         ▼
 Kernel IP stack (client)
 dst=10.8.0.3 → local delivery
         │
         ▼
 Application receives reply
```

---

## 7. Server-side per-client routing

When a client connects `handle_connect` calls `manage_client_route(ip, true)`:

```
SIOCADDRT  →  ip route add 10.8.0.3/32 dev tun0
```

This is a host route so the kernel sends packets destined for exactly `10.8.0.3` through `tun0`, where `server_tun_io` picks them up and dispatches to the right peer.

On disconnect `handle_close` calls `manage_client_route(ip, false)`:

```
SIOCDELRT  →  ip route del 10.8.0.3/32 dev tun0
```

---

## 8. VPN status Lua file

The client writes connection state to `--status` path (default `/run/vpn_status.lua`):

```lua
-- auto-generated by lua_file::write_table
return {
  vpn_status = {
    service_ip = "10.8.0.3",
    tun_if     = "tun0",
    status     = "Connected",   -- or "Down"
    timestamp  = 1700000000,
  },
}
```

Read it from a Lua script via `require()` or load it with `lua_engine`.

---

## 9. Docker Compose — running with `marvel:release`

The `docs/docker-compose.yml` file runs both services from the `marvel:release` image
on a dedicated bridge network (`172.20.0.0/24`).

### Services

| Service          | Container IP  | Ports exposed         | Profile  |
|------------------|---------------|-----------------------|----------|
| `vpn-server`     | 172.20.0.2    | 1194 (tunnel), 58989 (gNMI) | default |
| `vpn-client`     | 172.20.0.3    | —                     | default  |
| `vpn-client-tls` | 172.20.0.4    | —                     | `tls`    |

Both services require `CAP_NET_ADMIN` and `/dev/net/tun` — declared once via
the `x-tun-caps` YAML anchor and merged into each service with `<<: *tun-caps`.

### Start plain (server + client)

```bash
docker compose -f docs/docker-compose.yml up
```

### Start with TLS client as well

```bash
# Generate certs first (self-signed example)
openssl req -x509 -newkey rsa:2048 -keyout docs/certs/key.pem \
            -out docs/certs/cert.pem -days 365 -nodes

docker compose -f docs/docker-compose.yml --profile tls up
```

The TLS client mounts `./certs/` read-only at `/certs` inside the container and
passes `--tls=true --cert=/certs/cert.pem --key=/certs/key.pem --ca=/certs/ca.pem`.

### How Docker DNS wires client → server

The client uses `--server=vpn-server`. Docker's embedded DNS resolves the service
name `vpn-server` to `172.20.0.2` automatically — no hardcoded IPs needed.

### Network topology

```
Docker bridge: vpn-net (172.20.0.0/24)

  ┌─────────────────────────┐      TCP 1194       ┌─────────────────────────┐
  │  vpn-client             │ ──────────────────► │  vpn-server             │
  │  eth0: 172.20.0.3       │                     │  eth0: 172.20.0.2       │
  │  tun0: 10.8.0.3/24      │ ◄── tunnel frames ──│  tun0: 10.8.0.1/24      │
  └─────────────────────────┘                     └─────────────────────────┘
          │  (virtual IP traffic)                          │
          └──────────────── 10.8.0.0/24 ──────────────────┘
```

- `eth0` carries the encrypted/raw TCP tunnel frames between containers.
- `tun0` on each side carries the virtual IP traffic (10.8.0.0/24).
- The server adds a `/32` host route per connected client via `SIOCADDRT`
  so the kernel routes reply packets back through `tun0` to the right peer.

---

## 10. Client authentication via certificate CN

### How mutual TLS (mTLS) works

When TLS is enabled, the server requires every connecting client to present a
certificate signed by the configured CA (`--ca`).  OpenSSL rejects the TLS
handshake automatically if the client presents no certificate or one signed by
an unknown CA — no application code is needed for this.

After the handshake the server extracts the **Common Name (CN)** from the
client certificate and can use it for fine-grained authorisation.

```
Client                              Server
──────                              ──────
ClientHello  ──────────────────────►
             ◄────────────────────── ServerHello + server cert (CN=vpn-server)
client cert (CN=vpn-client) ───────►
             ◄────────────────────── Finished  (handshake OK)
BEV_EVENT_CONNECTED fires                    BEV_EVENT_CONNECTED fires
                                     openvpn_peer::handle_connect()
                                       extract_cn() → "vpn-client"
                                       check against allowed-CN list
                                       → accept / reject
```

### SSL_CTX configuration (tls_config.hpp)

```cpp
SSL_CTX_set_verify(ctx.get(),
                   SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT,
                   nullptr);
```

`SSL_VERIFY_PEER` — server requests client certificate.  
`SSL_VERIFY_FAIL_IF_NO_PEER_CERT` — handshake fails if client sends no cert.

### CN extraction (openvpn_peer.cpp)

```cpp
SSL   *ssl  = bufferevent_openssl_get_ssl(get_bufferevt());
X509  *cert = SSL_get_peer_certificate(ssl);          // NULL for plain TCP
char   cn[256]{};
X509_NAME_get_text_by_NID(X509_get_subject_name(cert),
                           NID_commonName, cn, sizeof(cn));
X509_free(cert);
// cn now holds e.g. "vpn-client"
```

`bufferevent_openssl_get_ssl()` returns `nullptr` for plain-TCP bufferevents,
so the same code path is safe for non-TLS connections.

### Adding an allowed-CN list

In `openvpn_server.hpp` add a set of permitted CNs:

```cpp
std::set<std::string> m_allowed_cns; // empty = allow all
```

Populate it from a Lua config file via `lua_engine` at startup:

```lua
-- /app/command/allowed_clients.lua
return {
  allowed_clients = {
    "vpn-client",
    "vpn-client-2",
  }
}
```

In `openvpn_peer::handle_connect` after extracting the CN:

```cpp
if (!m_parent->is_cn_allowed(m_peer_cn)) {
  std::cerr << "[openvpn_peer] rejected CN=\"" << m_peer_cn << "\"\n";
  return -1;   // closes the connection
}
```

### Test certificates (CN mapping)

| File                    | CN           | Role                        |
|-------------------------|--------------|-----------------------------|
| `certs/ca.pem`          | Marvel-CA    | Trust anchor (both sides)   |
| `certs/server.pem/.key` | vpn-server   | Server identity             |
| `certs/client.pem/.key` | vpn-client   | Client identity (allowed CN)|

To add a second client:

```bash
openssl genrsa -out certs/client2.key 2048
openssl req -new -key certs/client2.key -subj "/CN=vpn-client-2" -out /tmp/c2.csr
openssl x509 -req -in /tmp/c2.csr -CA certs/ca.pem -CAkey certs/ca.key \
             -CAcreateserial -out certs/client2.pem -days 3650 -sha256
```

Then add `"vpn-client-2"` to `allowed_clients.lua` — no server restart required
if the server reloads the Lua file via `lua_engine` on `inotify` change.
