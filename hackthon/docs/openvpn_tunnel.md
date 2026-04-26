# OpenVPN Tunnel — Running in Docker & Packet Flow

## 1. Why TUN inside Docker needs special privileges

TUN interfaces are kernel virtual network devices. Creating one requires:

- `CAP_NET_ADMIN` — to call `TUNSETIFF`, `SIOCSIFADDR`, `SIOCSIFNETMASK`, `SIOCSIFFLAGS`, `SIOCADDRT`
- `/dev/net/tun` device — must be accessible inside the container

Without these the `open("/dev/net/tun")` or ioctl calls silently fail and `m_tun_fd` stays `-1`.

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
