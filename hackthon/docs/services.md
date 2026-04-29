# Docker Compose Services

Reference for every service defined in `docs/docker-compose.yml` and the
`docs/docker-compose.mqtt.yml` overlay.  All services share the single image
`marvel:release` built from this repository.

---

## Network Layout

Two Docker bridge networks are used:

| Network | Subnet | Purpose |
|---------|--------|---------|
| `vpn-net` | `172.20.0.0/24` | All services that participate in the VPN or MQTT stack |
| `app-net` | `172.21.0.0/24` | Private link between `vpn-client` and `gnmi-server-svc`; invisible to `vpn-net` |

```
vpn-net  172.20.0.0/24
┌────────────────────────────────────────────────────────────────────────┐
│  vpn-server            172.20.0.2    (tun0 10.8.0.1)                   │
│  vpn-client            172.20.0.3    (tun0 10.8.0.X)  ──┐             │
│  vpn-server-tls        172.20.0.5    (tun0 10.9.0.1)    │ also on      │
│  vpn-client-tls        172.20.0.6    (tun0 10.9.0.X)    │ app-net      │
│  gnmi-server           172.20.0.7                        │             │
│  gnmi-server-tls       172.20.0.8                        │             │
│  gnmi-client           172.20.0.9                        │             │
│  gnmi-client-tls       172.20.0.10                       │             │
│  mosquitto             172.20.0.11                       │             │
│  gnmi-client-svc       172.20.0.12   (MQTT relay)       │             │
│  cli                   172.20.0.18   (interactive REPL)  │             │
│  registration-svc      172.20.0.13   (58988 plain, 58992 TLS)          │
│  telemetry-svc         172.20.0.14   (58989 plain, 58993 TLS)          │
│  openvpn-server        172.20.0.15   (system openvpn, port 11194)      │
│  openvpn-client        172.20.0.16   (system openvpn)   ──┐            │
│  openvpn-server-mqtt   172.20.0.17   (openvpn + MQTT)      │ also on   │
└─────────────────────────────────────────────────────────────┼──────────┘
                                                              │
app-net  172.21.0.0/24                                        │
┌─────────────────────────────────────────────────────────────┘
│  vpn-client        172.21.0.3    (eth1, iptables DNAT proxy)
│  gnmi-server-svc   172.21.0.5    (gNMI, app-net only)
│  openvpn-client    172.21.0.6    (eth1, iptables DNAT proxy)
└──────────────────────────────────────────────────────────────
```

---

## Service Reference

### `vpn-server`

| | |
|-|-|
| **Mode** | `--mode=server` |
| **IP** | `172.20.0.2` |
| **Ports (host→container)** | `1194:1194` (VPN tunnel), `58989:58989` (gNMI push) |
| **Profile** | `vpn` |
| **Healthcheck** | `/proc/net/tcp` contains `:04AA` (port 1194) |

Opens a kernel TUN interface (`tun0`, `10.8.0.1/24`) and accepts VPN clients on
TCP 1194.  After a client connects it pushes a gNMI `Get` to the client's gNMI
server (port 58989 through the tunnel) after `--gnmi-push-delay` seconds.

When started with `--mqtt-host` (mqtt overlay), it additionally subscribes to
`fwd/#` on the MQTT broker and forwards each message as an HTTP/2 gRPC call into
the tunnel.  See [gnmi-mqtt-flow.md](gnmi-mqtt-flow.md) for the full flow.

---

### `vpn-client`

| | |
|-|-|
| **Mode** | `--mode=client` |
| **IP** | `172.20.0.3` (vpn-net), `172.21.0.3` (app-net) |
| **Profile** | `vpn` |
| **Depends on** | `vpn-server` healthy |

Connects to `vpn-server:1194`, receives a virtual IP (e.g. `10.8.0.3/24`),
brings up `tun0`, and writes state to `/run/vpn_status.lua`.  Then starts a
`socat` proxy that listens on the assigned virtual IP at port 58989 and
forwards traffic to `gnmi-server-svc` at `172.21.0.5:58989` on `app-net`.
This is the DNAT equivalent: gNMI requests injected through the tunnel by
`vpn-server` reach `gnmi-server-svc` transparently.

---

### `vpn-server-tls` / `vpn-client-tls`

| | |
|-|-|
| **Ports** | `1195:1194` (TLS tunnel), `58990:58989` (gNMI) |
| **Profile** | `tls` |

TLS variant of the plain VPN pair.  Certs are baked into the image at
`/app/certs/`.  Host port 1195 avoids collision with the plain vpn-server.

```bash
docker compose -f docs/docker-compose.yml --profile tls up
```

---

### `registration-svc`

| | |
|-|-|
| **Mode** | `--mode=gnmi-server` |
| **IP** | `172.20.0.13` |
| **Ports (host→container)** | `58988:58988` (plain), `58992:58992` (TLS) |
| **Profile** | `registration`, `registration-tls`, `mqtt`, `mqtt-tls` |
| **Healthcheck** | `/proc/net/tcp` contains `:E66C` (58988) and `:E670` (58992) |

Dedicated gNMI server for **device registration**.  Runs two listeners
simultaneously on the same container — plain TCP on port 58988 and mutual-TLS
on port 58992 — so plain and TLS clients can reach it without a separate service.
Certs are baked into the image at `/app/certs/`.

Typical usage via the VPN tunnel:
- `/gnmi.gNMI/Set` — device registers itself (writes identity/config leaf)
- `/gnmi.gNMI/Get` — device or operator queries registration state

`docker-compose.mqtt.yml` adds a `service_healthy` dependency so `vpn-server`
does not start until this service is ready.

---

### `telemetry-svc`

| | |
|-|-|
| **Mode** | `--mode=gnmi-server` |
| **IP** | `172.20.0.14` |
| **Ports (host→container)** | `58991:58989` (plain), `58993:58993` (TLS) |
| **Profile** | `telemetry`, `telemetry-tls`, `mqtt`, `mqtt-tls` |
| **Healthcheck** | `/proc/net/tcp` contains `:E66D` (58989) and `:E671` (58993) |

Dedicated gNMI server for **telemetry collection**.  Same dual-listener design
as `registration-svc` — plain TCP on port 58989 and TLS on port 58993.  Host
port 58991 maps the plain TCP port to avoid collision with the standalone
`gnmi-server` service which already claims `58989:58989`.

Typical usage via the VPN tunnel:
- `/gnmi.gNMI/Set` — device pushes a telemetry sample
- `/gnmi.gNMI/Get` — operator subscribes to or polls a telemetry stream

---

### `gnmi-server`

| | |
|-|-|
| **Mode** | `--mode=gnmi-server` |
| **IP** | `172.20.0.7` |
| **Port** | `58989:58989` |
| **Profile** | `gnmi` |

Standalone gNMI server for direct (non-VPN) gRPC access.  Used by
`gnmi-client` for probe testing.

---

### `gnmi-server-tls`

| | |
|-|-|
| **Mode** | `--mode=gnmi-server --gnmi-tls=true` |
| **IP** | `172.20.0.8` |
| **Port** | `58990:58989` |
| **Profile** | `gnmi-tls` |

TLS variant of `gnmi-server`.  Certs at `/app/certs/`.

---

### `gnmi-client` / `gnmi-client-tls`

| | |
|-|-|
| **Mode** | `--mode=client --gnmi-probe=true` |
| **Profile** | `gnmi` / `gnmi-tls` |

VPN client that probes the gNMI server after the tunnel is established.
Used to verify end-to-end gNMI over VPN connectivity.

---

### `mosquitto`

| | |
|-|-|
| **Image** | `eclipse-mosquitto:2` |
| **IP** | `172.20.0.11` |
| **Port** | `1883:1883` |
| **Profile** | `mqtt` |
| **Config** | `docs/mosquitto.conf` (mounted read-only) |
| **Healthcheck** | `mosquitto_pub -h localhost -t health -m ping` |

MQTT broker at the centre of the 3-stage gNMI-over-MQTT flow.
All four MQTT participants (`cli_app`, `gnmi-client-svc`, `vpn-server`,
and back to `cli_app`) connect to this broker.

---

### `gnmi-server-svc`

| | |
|-|-|
| **Mode** | `--mode=gnmi-server` |
| **IP** | `172.21.0.5` (app-net only) |
| **Profile** | `mqtt` |

The target gNMI server in the MQTT flow.  Lives exclusively on `app-net`
and is not reachable from `vpn-net` directly; traffic reaches it only
via the `vpn-client` socat proxy.

---

### `cli`

| | |
|-|-|
| **Binary** | `/app/cli_app` |
| **IP** | `172.20.0.18` |
| **Profile** | `openvpn` |
| **Depends on** | `mosquitto` healthy, `gnmi-client-svc` started |

Interactive readline REPL (`Marvel>` prompt).  Connects to the MQTT broker using
the `MQTT_HOST` / `MQTT_PORT` environment variables (set automatically by Compose).

Available commands inside the REPL:

| Command | gNMI RPC | MQTT topic |
|---------|----------|------------|
| `gnmi_get <target-vip> <path>` | `Get` | `cli/<vip>` |
| `gnmi_update <target-vip> <path> <val>` | `Set (Update)` | `cli/<vip>` |
| `gnmi_replace <target-vip> <path> <val>` | `Set (Replace)` | `cli/<vip>` |
| `gnmi_delete <target-vip> <path>` | `Set (Delete)` | `cli/<vip>` |

Responses arrive on `cli_resp/<vip>` via `gnmi-client-svc`.  The container has
`stdin_open: true` and `tty: true` so you can attach to it interactively:

```bash
# After the stack is running:
docker compose -f docs/docker-compose.yml --profile openvpn attach cli

# Detach without stopping the container:
Ctrl-P  Ctrl-Q
```

`restart: "no"` — the container is not restarted automatically; re-attach or
`docker compose ... start cli` to bring it back after detach or exit.

---

### `gnmi-client-svc`

| | |
|-|-|
| **Mode** | `--mode=gnmi-mqtt-client` |
| **IP** | `172.20.0.12` |
| **Profile** | `mqtt`, `openvpn` |
| **Depends on** | `mosquitto` healthy |

The MQTT relay.  Subscribes to `cli/#` and `resp/#` and re-publishes:

| Received topic | Re-published as | Direction |
|----------------|-----------------|-----------|
| `cli/<vip>` | `fwd/<vip>` | CLI → vpn-server |
| `resp/<vip>` | `cli_resp/<vip>` | vpn-server → CLI |

---

## Standard OpenVPN Wrapper Services

These services wrap the **system `openvpn` binary** (installed via the `openvpn`
package in the runtime image) rather than the custom VPN protocol used by
`vpn-server` / `vpn-client`.  They are fully compatible with any standard
OpenVPN server/client.

### How it works

```
openvpn_server                          openvpn_client
──────────────                          ──────────────
fork + execvp("openvpn --server …")    fork + execvp("openvpn --client …")
stdout/stderr → pipe → proc_io          stdout/stderr → pipe → proc_io
                                          │
management interface (TCP 7505)           │  parse log lines for:
  ← status 2  (every 5 s)                │    "Initialization Sequence Completed"
  → ROUTING TABLE rows                   │    "addr add local <vip>"
  diff vs previous poll                  │    "ifconfig tun0 <vip>"
    new VIP  → on_client_connect         │    "ifconfig_local=<vip>"
    gone VIP → on_client_disconnect      │
       │                                 │  tunnel up → iptables PREROUTING DNAT:
       │  mqtt_io subscriber fwd/<vip>   │    <vip>:80    → fwd-host:80
       │  gnmi_client::push_async        │    <vip>:443   → fwd-host:443
       │  response → resp/<vip>          │    <vip>:58989 → fwd-host:58989
```

The `openvpn_server` binary never touches gNMI itself — it only monitors
which VIPs are connected and wires up per-client MQTT subscribers.  The
`openvpn_client` binary only manages the tunnel and the iptables rules;
the actual gNMI server (`gnmi-server-svc`) runs as a separate container
on `app-net`.

---

### `openvpn-server`

| | |
|-|-|
| **Binary** | `/app/openvpn_server` |
| **IP** | `172.20.0.15` |
| **Ports (host→container)** | `11194:1194` (OpenVPN TCP tunnel) |
| **Profile** | `openvpn` |
| **Depends on** | `mosquitto` healthy |
| **Healthcheck** | `/proc/net/tcp` contains `:1D51` (management port 7505) |

Spawns `openvpn --server --proto tcp-server --port 1194 --management 127.0.0.1 7505`.
The management interface port (7505) is used as the healthcheck signal — it opens
only after openvpn has finished initialising, which is more reliable than checking
the VPN port itself.  MQTT forwarding is always enabled: each connected client gets
a per-VIP subscriber on `fwd/<vip>` and responses are published on `resp/<vip>`.

Key flags:

| Flag | Default | Description |
|------|---------|-------------|
| `--port` | `1194` | OpenVPN listen port |
| `--mgmt-port` | `7505` | Management interface port (local only) |
| `--tls` | `false` | Enable TLS (requires `--cert/--key/--ca`) |
| `--mqtt-host` | _(off)_ | MQTT broker — enables per-client gNMI forwarding |
| `--mqtt-port` | `1883` | MQTT broker port |
| `--gnmi-port` | `58989` | Port to connect to on each client for gNMI |

---

### `openvpn-client`

| | |
|-|-|
| **Binary** | `/app/openvpn_client` |
| **IP** | `172.20.0.16` (vpn-net), `172.21.0.6` (app-net) |
| **Profile** | `openvpn` |
| **Depends on** | `openvpn-server` healthy |

Spawns `openvpn --client --proto tcp-client --remote <server> <port>`.  Reads the
child's stdout through a pipe registered with libevent — no polling thread.

After detecting tunnel-up and the assigned VIP the binary installs kernel NAT rules:

```
iptables -t nat -A PREROUTING -d <vip> -p tcp --dport <port> -j DNAT --to-destination <fwd-host>:<port>
iptables -t nat -A POSTROUTING -j MASQUERADE
sysctl -w net.ipv4.ip_forward=1
```

Rules are removed automatically when the binary exits.

Key flags:

| Flag | Default | Description |
|------|---------|-------------|
| `--server` | `127.0.0.1` | OpenVPN server address |
| `--port` | `1194` | OpenVPN server port |
| `--tls` | `false` | Enable TLS (requires `--cert/--key/--ca`) |
| `--fwd-host` | `127.0.0.1` | DNAT destination (address of `gnmi-server-svc`) |
| `--fwd-ports` | `80,443,58989` | Comma-separated ports to DNAT at the assigned VIP |
| `--status` | `/run/openvpn_status.lua` | Lua status file written on tunnel-up |

---

The full gNMI-over-MQTT flow using the standard OpenVPN backend:

```
cli_app  →  cli/<vip>  →  gnmi-client-svc  →  fwd/<vip>
                                                    ↓
                                           openvpn-server
                                                    ↓
                                     gnmi_client::push_async(<vip>:58989)
                                                    ↓
                                          OpenVPN tunnel (TCP 1194)
                                                    ↓
                                           openvpn-client (iptables DNAT)
                                                    ↓
                                           gnmi-server-svc:58989
                                                    ↓
                                      resp/<vip>  →  gnmi-client-svc  →  cli_app
```

---

## Port / Address Summary

| Service | Container IP | Internal port | Host port | Healthcheck hex |
|---------|-------------|---------------|-----------|----------------|
| `vpn-server` | 172.20.0.2 | 1194, 58989 | 1194, 58989 | `:04AA` |
| `vpn-client` | 172.20.0.3 / 172.21.0.3 | — | — | — |
| `vpn-server-tls` | 172.20.0.5 | 1194, 58989 | 1195, 58990 | `:04AA` |
| `vpn-client-tls` | 172.20.0.6 | — | — | — |
| `gnmi-server` | 172.20.0.7 | 58989 | 58989 | — |
| `gnmi-server-tls` | 172.20.0.8 | 58989 | 58990 | — |
| `gnmi-client` | 172.20.0.9 | — | — | — |
| `gnmi-client-tls` | 172.20.0.10 | — | — | — |
| `mosquitto` | 172.20.0.11 | 1883 | 1883 | pub/sub probe |
| `gnmi-client-svc` | 172.20.0.12 | — | — | — |
| `cli` | 172.20.0.18 | — | — | — |
| `registration-svc` | 172.20.0.13 | 58988 (plain), 58992 (TLS) | 58988, 58992 | `:E66C` + `:E670` |
| `telemetry-svc` | 172.20.0.14 | 58989 (plain), 58993 (TLS) | 58991, 58993 | `:E66D` + `:E671` |
| `openvpn-server` | 172.20.0.15 | 1194 (VPN), 7505 (mgmt) | 11194 | `:1D51` |
| `openvpn-client` | 172.20.0.16 / 172.21.0.6 | — | — | — |
| `gnmi-server-svc` | 172.21.0.5 | 58989 | — | — |

---

## Profiles

| Profile | Services started |
|---------|-----------------|
| `vpn` | `vpn-server`, `vpn-client` |
| `tls` | + `vpn-server-tls`, `vpn-client-tls` |
| `gnmi` | + `gnmi-server`, `gnmi-client` |
| `gnmi-tls` | + `gnmi-server-tls`, `gnmi-client-tls` |
| `registration` | + `registration-svc` (plain 58988 + TLS 58992) |
| `registration-tls` | + `registration-svc` (same service, both ports) |
| `telemetry` | + `telemetry-svc` (plain 58989 + TLS 58993) |
| `telemetry-tls` | + `telemetry-svc` (same service, both ports) |
| `mqtt` | + `mosquitto`, `gnmi-server-svc`, `gnmi-client-svc`, `registration-svc`, `telemetry-svc`, `vpn-server` (with MQTT args via overlay) |
| `mqtt-tls` | + same services as `mqtt`; each service already handles TLS on its second port |
| `openvpn` | + `mosquitto`, `gnmi-server-svc`, `gnmi-client-svc`, `cli`, `openvpn-server` (with MQTT), `openvpn-client` |

---

## Startup Recipes

### Minimal VPN pair (plain TCP)
```bash
docker compose -f docs/docker-compose.yml up
```

### Full MQTT gNMI stack (recommended for CLI testing)
```bash
docker compose -f docs/docker-compose.yml \
               -f docs/docker-compose.mqtt.yml \
               --profile mqtt up
```
Starts: `mosquitto`, `vpn-server` (with `--mqtt-host`), `vpn-client`,
`gnmi-client-svc` (relay), `gnmi-server-svc`, `registration-svc`,
`telemetry-svc`.

### Registration service only
```bash
docker compose -f docs/docker-compose.yml --profile registration up
```

### Telemetry service only (plain TCP)
```bash
docker compose -f docs/docker-compose.yml --profile telemetry up
```

### Registration + Telemetry (both serve plain + TLS automatically)
```bash
docker compose -f docs/docker-compose.yml --profile registration-tls --profile telemetry-tls up
```
Each service listens on both its plain port and TLS port within the same container.

### Full MQTT gNMI stack (plain + TLS on every service)
```bash
docker compose -f docs/docker-compose.yml \
               -f docs/docker-compose.mqtt.yml \
               --profile mqtt up
```
`registration-svc` and `telemetry-svc` each serve both plain and TLS clients
with no additional services required.

### TLS tunnel
```bash
docker compose -f docs/docker-compose.yml --profile tls up
```

### Standard OpenVPN with MQTT gNMI forwarding (full stack)
```bash
docker compose -f docs/docker-compose.yml --profile openvpn up
```
Starts the complete standard-openvpn stack in one command:

| Service | Role |
|---------|------|
| `mosquitto` | MQTT broker (172.20.0.11:1883) |
| `gnmi-server-svc` | gNMI server on `app-net` (172.21.0.5:58989) |
| `gnmi-client-svc` | MQTT relay: `cli/<vip>` → `fwd/<vip>`, `resp/<vip>` → `cli_resp/<vip>` |
| `openvpn-server` | Wraps system openvpn server; per-client MQTT subscriber on `fwd/<vip>` |
| `openvpn-client` | Wraps system openvpn client; iptables DNAT → `gnmi-server-svc` |

Startup order enforced by `depends_on`:
```
mosquitto (healthy) → openvpn-server (healthy) → openvpn-client
                    → gnmi-client-svc
```

CLI workflow: `cli_app` publishes to `cli/<vip>`, `gnmi-client-svc` relays to
`fwd/<vip>`, `openvpn-server` forwards the gNMI request through the tunnel to
`<vip>:58989`, and the response comes back on `resp/<vip>`.

> **Note:** requires `NET_ADMIN` capability and `/dev/net/tun` — both are
> set automatically by the Compose service definitions.

### Standard OpenVPN with TLS
Add `--tls=true` and cert paths to `openvpn-server` / `openvpn-client` command in
the Compose file (or override via a local `docker-compose.override.yml`):

```yaml
# docker-compose.override.yml
services:
  openvpn-server:
    command:
      - /app/openvpn_server
      - --port=1194
      - --tls=true
      - --cert=/app/certs/server.pem
      - --key=/app/certs/server.key
      - --ca=/app/certs/ca.pem
  openvpn-client:
    command:
      - /app/openvpn_client
      - --server=openvpn-server
      - --port=1194
      - --tls=true
      - --cert=/app/certs/client.pem
      - --key=/app/certs/client.key
      - --ca=/app/certs/ca.pem
      - --fwd-host=172.21.0.5
      - --fwd-ports=80,443,58989
```

Then run:
```bash
docker compose -f docs/docker-compose.yml \
               -f docker-compose.override.yml \
               --profile openvpn up
```

---

## Healthcheck Hex Reference

Docker healthchecks for gNMI services grep `/proc/net/tcp` for the listening
port in hex (big-endian, zero-padded to 4 digits):

| Port | Hex | Service |
|------|-----|---------|
| 1194 | `04AA` | `vpn-server`, `vpn-server-tls` |
| 7505 | `1D51` | `openvpn-server`, `openvpn-server-mqtt` (management interface) |
| 58988 | `E66C` | `registration-svc` (plain) |
| 58989 | `E66D` | `telemetry-svc` (plain) |
| 58992 | `E670` | `registration-svc` (TLS) |
| 58993 | `E671` | `telemetry-svc` (TLS) |
