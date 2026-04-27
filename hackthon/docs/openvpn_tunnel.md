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
docker build -t marvel:release .
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

---

## 11. Verifying tunnel connectivity and gNMI end-to-end

### Step 1 — check the tunnel status file

The client writes `/run/vpn_status.lua` after `IP_ASSIGN` is processed:

```bash
docker exec vpn-client cat /run/vpn_status.lua
```

Expected output when connected:

```lua
-- auto-generated by lua_file::write_table
return {
  vpn_status = {
    service_ip = "10.8.0.3",
    tun_if     = "tun0",
    status     = "Connected",
    timestamp  = 1700000000,
  },
}
```

### Step 2 — verify L3 connectivity through the tunnel

```bash
docker exec vpn-client ping -c 3 10.8.0.1
```

A reply means IP packets are flowing through `tun0` on the client side,
through the TCP tunnel, and out of `tun0` on the server side.

### Step 3 — gNMI probe through the tunnel (`--gnmi-probe`)

The `--gnmi-probe=true` flag adds an in-process gNMI `Get` check after the
VPN tunnel comes up.  The tunnel stays open afterwards.

#### How it works — phased event loop

```
main()
  │
  ├─ openvpn_client vpn_client(...)    ← registers bufferevent in libevent
  │
  ├─ Phase 1: while (!vpn_client.ip_assigned())
  │             event_base_loop(base, EVLOOP_ONCE)
  │           ↳ drives the VPN bufferevent; exits when IP_ASSIGN arrives
  │             and tun0 is configured
  │
  ├─ Phase 2: gnmi_client::call("10.8.0.1", 58989, "/gnmi.gNMI/Get", req)
  │           ↳ also loops event_base_loop(EVLOOP_ONCE) until response
  │             SAFE — Phase 1 has already returned, no nested dispatch
  │           ↳ dump_gnmi_response() prints grpc_status + notification count
  │
  └─ Phase 3: event_base_dispatch(base)   ← run_evt_loop{}()
              ↳ keeps the VPN tunnel alive indefinitely
```

`event_base_loop(EVLOOP_ONCE)` is **not** re-entrant.  The phased approach is
safe because no phase calls it while another phase is still inside it.

#### Run with probe

```bash
# Plain (no TLS)
docker run -d --name vpn-client --cap-add NET_ADMIN --device /dev/net/tun \
  --network vpn-net marvel:release \
  /app/app --mode=client --server=vpn-server --port=1194 \
           --status=/run/vpn_status.lua \
           --gnmi-probe=true --server-vip=10.8.0.1 --gnmi-port=58989
```

Or with docker compose (add the flags to the client command):

```yaml
  vpn-client:
    command:
      - /app/app
      - --mode=client
      - --server=vpn-server
      - --port=1194
      - --status=/run/vpn_status.lua
      - --gnmi-probe=true
      - --server-vip=10.8.0.1
      - --gnmi-port=58989
```

#### Expected log output

```
[main] mode=client server=vpn-server port=1194 tls=OFF gnmi-probe=ON
[openvpn_client] connecting to vpn-server:1194 tls=OFF
[main] waiting for VPN tunnel...
[openvpn_client] connected to 172.20.0.2, waiting for IP_ASSIGN
[openvpn_client] IP_ASSIGN: 10.8.0.3 255.255.255.0
[openvpn_client] tun0 configured: 10.8.0.3/24 UP
[main] tunnel up, assigned=10.8.0.3 probing 10.8.0.1:58989
[gnmi-probe] grpc_status=0 notification_count=1
  [0] timestamp=0 update_count=0
[main] probe done, tunnel remains open
```

`grpc_status=0` confirms that:
- The gNMI `Get` request left the client through `tun0`
- It was routed through the TCP tunnel to the server
- The server's kernel delivered it to the gNMI service (port 58989)
- The response travelled back through the same path

### Why gNMI Subscribe shows `grpc_status=12` (UNIMPLEMENTED)

The server's `grpc_session` handles only unary RPCs today.
`/gnmi.gNMI/Subscribe` is registered with status `12 = UNIMPLEMENTED`.
To get server-push notifications, the streaming Subscribe RPC must be
implemented in `grpc_session.cpp` and `client_app.cpp`.

---

## 12. `~evt_io` dtor log lines at shutdown

### What you see

```
vpn-server-1  | Fn:~evt_io:219 dtor
vpn-server-1  | Fn:~evt_io:219 dtor
vpn-server-1  | Fn:~evt_io:219 dtor
```

### Why it happens

`openvpn_server`, `openvpn_peer`, and `server_tun_io` all inherit from `evt_io`.
When the server process exits (e.g. `docker compose down` sends SIGTERM) its
destructor runs in this order:

```
~openvpn_server()
  │
  ├─ m_server_tun_io.reset()   → ~server_tun_io  → ~evt_io   ← dtor #1
  │
  ├─ m_peers.clear()           → ~openvpn_peer   → ~evt_io   ← dtor #2  (one per connected client)
  │                                                            (N dtors for N clients)
  │
  └─ [base class]              → ~evt_io (listener itself)    ← dtor #3
```

So **3 lines = 1 connected client at shutdown** (server-tun + 1 peer + listener).
With *N* clients you will see *N + 2* dtor lines.  This is normal — each
`evt_io` subobject releases its `bufferevent` / `evconnlistener` RAII handle
and logs the line.

### Suppressing the log line in production

The `std::cout` in `~evt_io` is a debug aid.  Remove or guard it with a
compile-time flag before shipping:

```cpp
// framework.hpp — ~evt_io
~evt_io() {
  m_buffer_evt_p.reset(nullptr);
  m_listener_p.reset(nullptr);
#ifndef NDEBUG
  std::cout << "Fn:" << __func__ << ":" << __LINE__ << " dtor\n";
#endif
}
```

Build with `-DNDEBUG` (the default for release builds via CMake
`-DCMAKE_BUILD_TYPE=Release`) to silence it.

---

## 13. gNMI server and client — TLS configuration

### Independent TLS flags

VPN tunnel TLS and gNMI TLS are **fully independent**.  Each has its own
flag set so all four combinations are valid:

| VPN TLS | gNMI TLS | Use case |
|---------|----------|----------|
| OFF     | OFF      | development / lab (default) |
| ON      | OFF      | encrypted tunnel, plain gNMI (trusted LAN) |
| OFF     | ON       | direct gNMI over TLS, plain VPN |
| ON      | ON       | fully encrypted — production recommended |

| Flag | Scope |
|------|-------|
| `--tls` / `--cert` / `--key` / `--ca` | OpenVPN tunnel (port 1194) |
| `--gnmi-tls` / `--gnmi-cert` / `--gnmi-key` / `--gnmi-ca` | gNMI listener / probe (port 58989) |

### Server mode

```bash
# Plain gNMI (default)
/app/app --mode=server

# gNMI with TLS
/app/app --mode=server \
  --gnmi-tls=true \
  --gnmi-cert=/app/certs/server.pem \
  --gnmi-key=/app/certs/server.key \
  --gnmi-ca=/app/certs/ca.pem        # also enables mTLS client-cert check

# VPN TLS + gNMI TLS (fully encrypted)
/app/app --mode=server \
  --tls=true --cert=/app/certs/server.pem --key=/app/certs/server.key --ca=/app/certs/ca.pem \
  --gnmi-tls=true --gnmi-cert=/app/certs/server.pem --gnmi-key=/app/certs/server.key \
  --gnmi-ca=/app/certs/ca.pem
```

### Client mode (with gNMI probe)

```bash
# VPN plain, gNMI probe plain
/app/app --mode=client --server=vpn-server \
  --gnmi-probe=true --server-vip=10.8.0.1 --gnmi-port=58989

# VPN plain, gNMI probe TLS
/app/app --mode=client --server=vpn-server \
  --gnmi-probe=true --server-vip=10.8.0.1 --gnmi-port=58989 \
  --gnmi-tls=true \
  --gnmi-cert=/app/certs/client.pem \
  --gnmi-key=/app/certs/client.key \
  --gnmi-ca=/app/certs/ca.pem

# VPN TLS + gNMI probe TLS
/app/app --mode=client --server=vpn-server \
  --tls=true --cert=/app/certs/client.pem --key=/app/certs/client.key --ca=/app/certs/ca.pem \
  --gnmi-probe=true --server-vip=10.8.0.1 --gnmi-port=58989 \
  --gnmi-tls=true --gnmi-cert=/app/certs/client.pem \
  --gnmi-key=/app/certs/client.key --gnmi-ca=/app/certs/ca.pem
```

### How it works in code

```
--mode=server
  │
  ├─ tls_config gnmi_tls{ --gnmi-tls, --gnmi-cert, --gnmi-key, --gnmi-ca }
  │
  └─ server svc_module("0.0.0.0", 58989, gnmi_tls)
       │
       └─ evt_io(host, port, gnmi_tls.build_server_ctx(), listener_tag{})
            │  build_server_ctx() returns nullptr when disabled → plain TCP
            │
            └─ on accept: wrap_accepted(fd)
                 ├─ TLS enabled → bufferevent_openssl_socket_new(fd, ssl, ACCEPTING)
                 └─ TLS disabled → bufferevent_socket_new(fd)
                 → connected_client(bev, peer, this)   ← same code path either way
```

```
--mode=client --gnmi-probe=true
  │
  ├─ tls_config gnmi_tls{ --gnmi-tls, --gnmi-cert, --gnmi-key, --gnmi-ca }
  │
  └─ gnmi_client::call(server_vip, port, rpc_path, req_pb, gnmi_tls)
       │
       └─ gnmi_connection : evt_io(host, port, gnmi_tls.build_client_ctx())
            │  build_client_ctx() returns nullptr when disabled → plain TCP
            └─ drives event_base_loop(EVLOOP_ONCE) until response arrives
```

### Docker Compose profiles

```bash
# Plain gNMI server + client probe
docker compose -f docs/docker-compose.yml --profile gnmi up

# TLS gNMI server + client probe
docker compose -f docs/docker-compose.yml --profile gnmi-tls up

# Everything (VPN + gNMI, both TLS)
docker compose -f docs/docker-compose.yml --profile tls --profile gnmi-tls up
```

### Note on gNMI Subscribe

`/gnmi.gNMI/Subscribe` returns `grpc_status=12` (UNIMPLEMENTED) — the
`grpc_session` currently handles only unary RPCs.  Streaming Subscribe
requires bidirectional HTTP/2 streams to be wired into `grpc_session.cpp`.

---

## 14. In-container network debugging — ifconfig, ip, tcpdump

The runtime image ships four diagnostic packages (added to the `runtime`
stage in `Dockerfile`):

| Package | Commands | Purpose |
|---------|----------|---------|
| `net-tools` | `ifconfig`, `route`, `netstat` | classic interface/route inspection |
| `iproute2` | `ip addr`, `ip route`, `ip link` | modern replacement |
| `iputils-ping` | `ping` | end-to-end L3 reachability through tunnel |
| `tcpdump` | `tcpdump` | live packet capture on any interface |

`tcpdump` requires `CAP_NET_RAW`.  The `x-tun-caps` anchor in
`docker-compose.yml` grants both `NET_ADMIN` (TUN/routing) and `NET_RAW`
(raw-socket capture) to every VPN service.

---

### 14.1 Inspect the TUN interface

After the client receives `IP_ASSIGN` it configures `tun0` and brings it up.
Confirm with either tool:

```bash
# classic
docker exec docs-vpn-client-1 ifconfig tun0

# modern
docker exec docs-vpn-client-1 ip addr show tun0
docker exec docs-vpn-client-1 ip route
```

Expected output (client assigned `10.8.0.3`):

```
tun0: flags=4305<UP,POINTOPOINT,RUNNING,NOARP,MULTICAST>  mtu 1500
        inet 10.8.0.3  netmask 255.255.255.0  destination 10.8.0.3
```

On the server side the TUN device carries the pool address (`10.8.0.1`):

```bash
docker exec docs-vpn-server-1 ifconfig tun0
docker exec docs-vpn-server-1 ip route   # host routes for every connected client
```

---

### 14.2 Capture packets with tcpdump

Two interfaces are interesting:

| Interface | What you see |
|-----------|-------------|
| `tun0` | Decrypted IP packets traversing the VPN tunnel (ICMP, TCP, etc.) |
| `eth0` | Raw TCP frames on port 1194 — your custom TYPE + 4-byte-len framing |

**Watch tunnel traffic (both ends):**

```bash
# client side — outbound packets entering the tunnel
docker exec docs-vpn-client-1 tcpdump -i tun0 -n -v

# server side — same packets arriving after kernel delivers them from tun0
docker exec docs-vpn-server-1 tcpdump -i tun0 -n -v
```

**Watch the raw VPN framing on the TCP connection:**

```bash
# hex+ASCII view of port-1194 frames — TYPE byte, 4-byte length, payload
docker exec docs-vpn-client-1 tcpdump -i eth0 -n port 1194 -X
```

**Save to a `.pcap` and open in Wireshark on the host:**

```bash
docker exec docs-vpn-client-1 tcpdump -i tun0 -n -w /tmp/tun0.pcap &
# ... reproduce traffic ...
kill %1   # stop the background tcpdump on the host
docker cp docs-vpn-client-1:/tmp/tun0.pcap ./tun0.pcap
# open tun0.pcap in Wireshark
```

Or write and copy in one step:

```bash
docker exec docs-vpn-client-1 tcpdump -i tun0 -n -c 200 -w /tmp/tun0.pcap \
  && docker cp docs-vpn-client-1:/tmp/tun0.pcap ./tun0.pcap
```

(`-c 200` stops after 200 packets so the command returns automatically.)

---

### 14.3 End-to-end ping through the tunnel

With the client at `10.8.0.3` and the server at `10.8.0.1`:

```bash
docker exec docs-vpn-client-1 ping -c 4 10.8.0.1
```

A successful reply confirms:
1. `tun0` is up and correctly addressed on the client.
2. The server's `tun0` is up and correctly addressed.
3. The kernel TUN reader on both sides forwards IP packets end-to-end.

To watch the ICMP exchange simultaneously:

```bash
# terminal 1 — server tun0
docker exec docs-vpn-server-1 tcpdump -i tun0 -n icmp

# terminal 2 — client fires pings
docker exec docs-vpn-client-1 ping -c 4 10.8.0.1
```

---

### 14.4 stdout buffering note (Docker)

When a container's stdout is not a TTY (the normal Docker case), the C++
runtime switches to 4 KiB block buffering.  Log lines that use `'\n'`
instead of `std::endl` sit silently in the buffer until it fills.

`main()` calls `std::cout << std::unitbuf;` at startup to force per-write
flushing for the entire process, so all `[openvpn_client]` / `[main]` lines
appear immediately in `docker compose logs`.

---

## 15. MQTT-based gNMI client/server architecture

### 15.1 Overview

This architecture decouples the gNMI client from the VPN topology entirely.
The gNMI client never needs to know a server IP or open a TCP connection —
it just publishes a protobuf message.  The VPN server acts as a relay that
routes the message through the correct tunnel peer based on the MQTT topic.

```
┌──────────────────┐   MQTT publish          ┌──────────────────────┐
│  gnmi-client-svc │  topic="10.8.0.3"       │     mosquitto        │
│  (172.20.0.12)   │ ──payload=gNMI proto──► │  broker 172.20.0.11  │
└──────────────────┘                         └──────────┬───────────┘
                                                        │ MQTT subscribe "#"
                                                        ▼
                                             ┌──────────────────────┐
                                             │  mqtt-vpn-server     │
                                             │  (172.20.0.13)       │
                                             │  tun0 = 10.8.0.1     │
                                             │                      │
                                             │  topic → virtual-IP  │
                                             │  → ip_pool lookup    │
                                             │  → openvpn_peer      │
                                             │    forward_data()    │
                                             └──────────┬───────────┘
                                                        │ VPN tunnel (TCP 11194)
                                                        ▼
                                             ┌──────────────────────┐
                                             │  mqtt-vpn-client     │
                                             │  eth0: 172.20.0.14   │
                                             │  tun0: 10.8.0.3      │
                                             │  eth1: 172.21.0.3    │
                                             │                      │
                                             │  iptables DNAT:      │
                                             │  10.8.0.3:58989      │
                                             │    → 172.21.0.5:58989│
                                             └──────────┬───────────┘
                                                        │ app-net (172.21.0.0/24)
                                                        ▼
                                             ┌──────────────────────┐
                                             │  gnmi-server-svc     │
                                             │  (172.21.0.5)        │
                                             │  port 58989          │
                                             └──────────────────────┘
```

### 15.2 Networks

| Network   | Subnet           | Members                                              |
|-----------|------------------|------------------------------------------------------|
| `vpn-net` | 172.20.0.0/24    | mosquitto, mqtt-vpn-server, mqtt-vpn-client, gnmi-client-svc |
| `app-net` | 172.21.0.0/24    | mqtt-vpn-client, gnmi-server-svc                     |

`gnmi-server-svc` has no interface on `vpn-net` — it is reachable only
after `mqtt-vpn-client`'s DNAT rule is in place.

### 15.3 MQTT message format

| Field   | Value |
|---------|-------|
| topic   | Virtual IP the VPN server assigned to the target client (e.g. `10.8.0.3`) |
| payload | Raw protobuf bytes of a `gnmi.GetRequest` or `gnmi.SetRequest` |
| QoS     | 0 (fire-and-forget) |

The topic doubles as the routing key.  `mqtt-vpn-server` calls
`ip_pool::find_channel(topic)` to look up the `openvpn_peer` that owns
that virtual IP, then injects the payload as a TCP/IP packet into the tunnel.

### 15.4 Forwarding rule in mqtt-vpn-client

After `tun0` comes up the client runs:

```bash
# Enable IP forwarding in the kernel
sysctl -w net.ipv4.ip_forward=1

# Create the nat table and hooks (idempotent — nft ignores duplicates)
nft add table ip nat
nft add chain ip nat prerouting  '{ type nat hook prerouting  priority dstnat; }'
nft add chain ip nat postrouting '{ type nat hook postrouting priority srcnat; }'

# Redirect gNMI traffic arriving on the virtual IP to gnmi-server-svc
nft add rule ip nat prerouting  ip daddr 10.8.0.3 tcp dport 58989 dnat to 172.21.0.5:58989

# Masquerade so gnmi-server-svc sees mqtt-vpn-client as the source
nft add rule ip nat postrouting oifname eth1 masquerade
```

Verify the ruleset inside the container:

```bash
docker exec docs-mqtt-vpn-client-1 nft list ruleset
```

The virtual IP (`10.8.0.3`) is read from the Lua status file written by
the VPN client after `IP_ASSIGN` is processed:

```bash
lua5.4 -e "local t=dofile('/run/vpn_status.lua'); print(t.vpn_status.service_ip)"
```

### 15.5 Starting the MQTT-based stack

```bash
docker compose -f docs/docker-compose.yml --profile mqtt up
```

Start order enforced by `depends_on`:

```
mosquitto (healthy)
    └─► mqtt-vpn-server (healthy, subscribes to mosquitto)
            └─► mqtt-vpn-client (tunnel up, iptables rule installed)
                    └─► gnmi-client-svc (starts publishing)
```

`gnmi-server-svc` starts in parallel with the VPN services since it has
no dependency on the tunnel — it just listens on `app-net`.

### 15.6 Planned C++ integration points

The compose file documents the intended design.  Two new flags need to be
wired into the C++ app before the MQTT relay is fully functional:

| Flag | Where | What it does |
|------|-------|--------------|
| `--mqtt-host` / `--mqtt-port` | `openvpn_server` | On tunnel-ready, call `mosquitto_subscribe(client, "#")`. In the message callback, call `ip_pool::find_channel(topic)` and `openvpn_peer::forward_data(payload)`. |
| `--mode=gnmi-mqtt-client` | `main_app` | Publish a serialised `gnmi.GetRequest` to the MQTT broker using `mosquitto_publish(client, topic, payload)`. |

Both use `libmosquitto` (already added to the build and runtime images).
`mosquitto_clients` in the runtime image (`mosquitto_pub` / `mosquitto_sub`)
can be used to test the broker integration before the C++ code is wired in:

```bash
# Verify the broker is reachable and mqtt-vpn-server is subscribed
docker exec docs-mosquitto-1 \
  mosquitto_pub -h localhost -t "10.8.0.3" -m "hello" -q 0

# Watch what mqtt-vpn-server receives
docker exec docs-mqtt-vpn-server-1 \
  mosquitto_sub -h mosquitto -t "#" -v
```

### 15.7 End-to-end packet flow with MQTT relay

```
gnmi-client-svc
  mosquitto_publish(topic="10.8.0.3", payload=GetRequest{...})
        │
        ▼
  mosquitto broker
        │  fan-out
        ▼
  mqtt-vpn-server  on_mqtt_message(topic="10.8.0.3", payload)
    ip_pool::find_channel("10.8.0.3") → peer fd
    build IP packet: src=10.8.0.1 dst=10.8.0.3 dport=58989 data=payload
    openvpn_peer::forward_data(raw_ip_packet)
    wrap as [0x02][len][IP packet] → bufferevent → TCP 11194
        │
        ▼
  mqtt-vpn-client  openvpn_client::handle_read() → TYPE_DATA
    write(tun0_fd, raw_ip_packet)
        │
        ▼  kernel: dst=10.8.0.3, PREROUTING DNAT → 172.21.0.5:58989
        ▼
  gnmi-server-svc  connected_client::handle_read()
    grpc_session::recv() → dispatch GetRequest
    build GetResponse, send back
        │
        ▼  reverse path: kernel POSTROUTING src-NAT → tun0 → tunnel → server
        ▼
  mqtt-vpn-server  (receives reply via tunnel, can forward back to MQTT or
                    deliver directly to gnmi-client-svc on vpn-net)
```
