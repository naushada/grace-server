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
┌────────────────────────────────────────────────────────────────────┐
│  vpn-server        172.20.0.2    (tun0 10.8.0.1)                   │
│  vpn-client        172.20.0.3    (tun0 10.8.0.X)  ──┐             │
│  vpn-server-tls    172.20.0.5    (tun0 10.9.0.1)     │ also on     │
│  vpn-client-tls    172.20.0.6    (tun0 10.9.0.X)     │ app-net     │
│  gnmi-server       172.20.0.7                         │             │
│  gnmi-server-tls   172.20.0.8                         │             │
│  gnmi-client       172.20.0.9                         │             │
│  gnmi-client-tls   172.20.0.10                        │             │
│  mosquitto         172.20.0.11                        │             │
│  gnmi-client-svc   172.20.0.12   (MQTT relay)        │             │
│  registration-svc  172.20.0.13   (gNMI port 58988)   │             │
│  telemetry-svc     172.20.0.14   (gNMI port 58989)   │             │
└────────────────────────────────────────────────────────────────────┘
                                                        │
app-net  172.21.0.0/24                                  │
┌────────────────────────────────────────────────────────┘
│  vpn-client        172.21.0.3    (eth1, socat proxy)
│  gnmi-server-svc   172.21.0.5    (gNMI, app-net only)
└──────────────────────────────────────────────────────
```

---

## Service Reference

### `vpn-server`

| | |
|-|-|
| **Mode** | `--mode=server` |
| **IP** | `172.20.0.2` |
| **Ports (host→container)** | `1194:1194` (VPN tunnel), `58989:58989` (gNMI push) |
| **Profile** | _(no profile — always started)_ |
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
| **Profile** | _(no profile — always started)_ |
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
| **Port (host→container)** | `58988:58988` |
| **Profile** | `registration`, `mqtt` |
| **Healthcheck** | `/proc/net/tcp` contains `:E66C` (58988 = 0xE66C) |

Dedicated gNMI server for **device registration**.  Runs the same binary as all
other gNMI services but on port 58988, keeping registration traffic clearly
separated from operational telemetry (58989).

Typical usage via the VPN tunnel:
- `/gnmi.gNMI/Set` — device registers itself (writes identity/config leaf)
- `/gnmi.gNMI/Get` — device or operator queries registration state

Included in the `mqtt` profile so it is present whenever the full MQTT stack is
running.  `docker-compose.mqtt.yml` adds a `service_healthy` dependency so
`vpn-server` does not start until this service is ready.

---

### `telemetry-svc`

| | |
|-|-|
| **Mode** | `--mode=gnmi-server` |
| **IP** | `172.20.0.14` |
| **Port (host→container)** | `58991:58989` |
| **Profile** | `telemetry`, `mqtt` |
| **Healthcheck** | `/proc/net/tcp` contains `:E66D` (58989 = 0xE66D) |

Dedicated gNMI server for **telemetry collection**.  Same binary as
`registration-svc`, different IP (`172.20.0.14`) and host-side port (`58991`).
Host port 58991 is used to avoid collision with the standalone `gnmi-server`
service which already claims `58989:58989`.

Typical usage via the VPN tunnel:
- `/gnmi.gNMI/Set` — device pushes a telemetry sample
- `/gnmi.gNMI/Get` — operator subscribes to or polls a telemetry stream

Included in the `mqtt` profile alongside `registration-svc`.

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

### `gnmi-client-svc`

| | |
|-|-|
| **Mode** | `--mode=gnmi-mqtt-client` |
| **IP** | `172.20.0.12` |
| **Profile** | `mqtt` |
| **Depends on** | `mosquitto` healthy, `vpn-server` healthy, `vpn-client` started |

The MQTT relay.  Subscribes to `cli/#` and `resp/#` and re-publishes:

| Received topic | Re-published as | Direction |
|----------------|-----------------|-----------|
| `cli/<vip>` | `fwd/<vip>` | CLI → vpn-server |
| `resp/<vip>` | `cli_resp/<vip>` | vpn-server → CLI |

---

## Port / Address Summary

| Service | Container IP | Internal port | Host port | Healthcheck hex |
|---------|-------------|---------------|-----------|----------------|
| `vpn-server` | 172.20.0.2 | 1194, 58989 | 1194, 58989 | `:04AA` |
| `vpn-client` | 172.20.0.3 | — | — | — |
| `vpn-server-tls` | 172.20.0.5 | 1194, 58989 | 1195, 58990 | `:04AA` |
| `vpn-client-tls` | 172.20.0.6 | — | — | — |
| `gnmi-server` | 172.20.0.7 | 58989 | 58989 | — |
| `gnmi-server-tls` | 172.20.0.8 | 58989 | 58990 | — |
| `gnmi-client` | 172.20.0.9 | — | — | — |
| `gnmi-client-tls` | 172.20.0.10 | — | — | — |
| `mosquitto` | 172.20.0.11 | 1883 | 1883 | pub/sub probe |
| `gnmi-client-svc` | 172.20.0.12 | — | — | — |
| `registration-svc` | 172.20.0.13 | 58988 | 58988 | `:E66C` |
| `telemetry-svc` | 172.20.0.14 | 58989 | 58991 | `:E66D` |
| `gnmi-server-svc` | 172.21.0.5 | 58989 | — | — |

---

## Profiles

| Profile | Services started |
|---------|-----------------|
| _(none)_ | `vpn-server`, `vpn-client` |
| `tls` | + `vpn-server-tls`, `vpn-client-tls` |
| `gnmi` | + `gnmi-server`, `gnmi-client` |
| `gnmi-tls` | + `gnmi-server-tls`, `gnmi-client-tls` |
| `registration` | + `registration-svc` |
| `telemetry` | + `telemetry-svc` |
| `mqtt` | + `mosquitto`, `gnmi-server-svc`, `gnmi-client-svc`, `registration-svc`, `telemetry-svc`, `vpn-server` (with MQTT args via overlay) |

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

### Telemetry service only
```bash
docker compose -f docs/docker-compose.yml --profile telemetry up
```

### TLS tunnel
```bash
docker compose -f docs/docker-compose.yml --profile tls up
```

---

## Healthcheck Hex Reference

Docker healthchecks for gNMI services grep `/proc/net/tcp` for the listening
port in hex (big-endian, zero-padded to 4 digits):

| Port | Hex | Service |
|------|-----|---------|
| 1194 | `04AA` | `vpn-server`, `vpn-server-tls` |
| 58988 | `E66C` | `registration-svc` |
| 58989 | `E66D` | `telemetry-svc` |
