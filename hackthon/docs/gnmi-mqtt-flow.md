# gNMI over MQTT–VPN Flow

This document describes how a gNMI operation typed at the CLI reaches a gNMI
server that sits behind an OpenVPN tunnel, and explains the current **response
gap** (why the CLI does not yet receive the gNMI response).

---

## Architecture Overview

Three long-lived processes must be running simultaneously:

| Process | Binary / mode | Role |
|---------|--------------|------|
| **CLI** | `cli_app` | readline REPL; builds gNMI proto, publishes to MQTT |
| **Relay** | `grace-server --mode=gnmi-mqtt-client` | bridges `cli/#` → `fwd/#` on the MQTT broker |
| **VPN server** | `grace-server --mode=server` (with `--mqtt-host`) | subscribes `fwd/#`, injects gNMI request into VPN tunnel |
| **VPN client** | `grace-server --mode=client` | receives framed request; nftables DNAT → local gNMI server |
| **gNMI server** | `grace-server --mode=gnmi-server` | handles the RPC and returns a response |

---

## Request Flow (CLI → gNMI server)

```
┌─────────────────────────────────────────────────────────────────────┐
│ CLI (cli_app)                   readline.cpp: handle_gnmi_update()  │
│                                                                     │
│  gnmi_update target=<vip> path=... value=...                        │
│    1. build gnmi::SetRequest proto                                  │
│    2. SerializeToString() → req_pb                                  │
│    3. mqtt_gnmi_publish(<vip>, "/gnmi.gNMI/Set", req_pb)            │
│         payload = "/gnmi.gNMI/Set\0<proto_bytes>"                   │
│         topic   = "cli/<vip>"                                       │
│         transport: raw mosquitto_publish (g_mosq, blocking)         │
└───────────────────┬─────────────────────────────────────────────────┘
                    │  MQTT broker
                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│ Relay  (--mode=gnmi-mqtt-client)         main_app.cpp:202           │
│                                                                     │
│  mqtt_io relay("broker", 1883, "gnmi-client-svc", relay_msg_cb)     │
│  relay.subscribe("cli/#")                                           │
│                                                                     │
│  relay_msg_cb:                                                      │
│    topic "cli/<ip>" → fwd_topic = "fwd/<ip>"                        │
│    mosquitto_publish(fwd_topic, same payload, ...)                  │
└───────────────────┬─────────────────────────────────────────────────┘
                    │  MQTT broker
                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│ VPN server  (--mode=server --mqtt-host=...)  openvpn_server.cpp     │
│                                                                     │
│  m_mqtt_io->subscribe("fwd/#")                                      │
│                                                                     │
│  on_mqtt_message():                                                 │
│    topic   "fwd/<vip>"  → dest_ip = <vip>                           │
│    payload split on '\0': rpc_path | proto_bytes                    │
│    gnmi_client::push_async(dest_ip, gnmi_port, rpc_path, proto_bytes)│
│      → opens TCP connection to <vip>:58989 through VPN tunnel       │
│      → sends HTTP/2 POST /<rpc_path> with gRPC-framed proto         │
└───────────────────┬─────────────────────────────────────────────────┘
                    │  OpenVPN tunnel  (tun0 ↔ tun0)
                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│ VPN client  (--mode=client)              openvpn_client.cpp         │
│                                                                     │
│  Receives TCP traffic destined for <vip>:58989 on its tun0.         │
│  nftables DNAT rule: <vip>:58989 → 127.0.0.1:58989                  │
└───────────────────┬─────────────────────────────────────────────────┘
                    │  loopback
                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│ gNMI server  (--mode=gnmi-server --gnmi-port=58989)                 │
│                                                                     │
│  Receives HTTP/2 stream, decodes gRPC frame, dispatches:            │
│    /gnmi.gNMI/Set   → SetRequest  → SetResponse                     │
│    /gnmi.gNMI/Get   → GetRequest  → GetResponse                     │
│  Sends response back over the same HTTP/2 connection.               │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Payload Wire Format

Every MQTT message in both `cli/<vip>` and `fwd/<vip>` uses the same layout:

```
[ rpc_path ][ 0x00 ][ proto_bytes ]
```

| Field | Example | Notes |
|-------|---------|-------|
| `rpc_path` | `/gnmi.gNMI/Set` | Null-terminated ASCII string |
| `0x00` | — | Separator byte |
| `proto_bytes` | `<binary>` | Serialised protobuf (SetRequest / GetRequest) |

The relay passes this payload through **unchanged** — it only rewrites the
MQTT topic.

---

## MQTT Topic Scheme

| Direction | Topic pattern | Publisher | Subscriber |
|-----------|--------------|-----------|------------|
| CLI → relay | `cli/<vip>` | `cli_app` (g_mosq) | relay (`mqtt_io`) |
| relay → VPN server | `fwd/<vip>` | relay (`mqtt_io`) | VPN server (`m_mqtt_io`) |

`<vip>` is the **virtual IP** assigned to the target VPN client (e.g. `10.8.0.2`).

---

## gNMI Operations Supported via CLI

| CLI command | gNMI proto built | RPC path |
|-------------|-----------------|----------|
| `gnmi_get` | `gnmi::GetRequest` | `/gnmi.gNMI/Get` |
| `gnmi_update` | `gnmi::SetRequest` (update[]) | `/gnmi.gNMI/Set` |
| `gnmi_replace` | `gnmi::SetRequest` (replace[]) | `/gnmi.gNMI/Set` |
| `gnmi_delete` | `gnmi::SetRequest` (delete[]) | `/gnmi.gNMI/Set` |

### Example

```
# In cli_app (MQTT_HOST and MQTT_PORT env vars must be set):
Marvel> gnmi_update target=10.8.0.2 path=/interfaces/interface[name=eth0]/config value={"enabled":true}
Marvel> gnmi_get    target=10.8.0.2 path=/interfaces/interface[name=eth0]/state/oper-status
```

---

## Current Limitation — No Response Path

`gnmi_client::push_async` is **fire-and-forget**: the gNMI server sends a
`SetResponse` / `GetResponse` back over the HTTP/2 connection opened by
`push_async`, but that response is discarded — it is never published back to
MQTT and never returned to the CLI.

```
gNMI server → SetResponse  ──►  push_async (gnmi_connection)
                                        │
                                     DROPPED  ← no MQTT publish, no callback
```

The CLI therefore receives no confirmation that the operation succeeded or
failed.

### What would be needed to close the loop

1. **`push_async` response callback** — add a
   `std::function<void(gnmi_client::response)>` parameter to `push_async`;
   `gnmi_connection::handle_read` calls it when the response frame is complete.

2. **VPN-server → MQTT publish** — in `on_mqtt_message`, pass a lambda that
   publishes the response to `resp/<vip>` on the broker.

3. **Relay forwarding** — relay subscribes to `resp/#` and bridges to
   `cli_resp/<vip>`.

4. **CLI subscription** — `cli_app` subscribes to `cli_resp/<client_id>` via
   a second `mqtt_io` (or via the existing `g_mosq`), waits for a message, and
   prints the decoded `SetResponse` / `GetResponse`.

---

## Sequence Diagram

```
cli_app          MQTT broker       relay           vpn-server       vpn-client      gnmi-server
   │                  │               │                 │                 │               │
   │──publish─────────►               │                 │                 │               │
   │  cli/<vip>       │               │                 │                 │               │
   │                  │──deliver──────►                 │                 │               │
   │                  │  cli/<vip>    │                 │                 │               │
   │                  │◄──publish─────│                 │                 │               │
   │                  │  fwd/<vip>    │                 │                 │               │
   │                  │───deliver─────────────────────► │                 │               │
   │                  │  fwd/<vip>    │                 │                 │               │
   │                  │               │          push_async(<vip>)        │               │
   │                  │               │                 │──TCP connect────────────────────►
   │                  │               │                 │  (through VPN)  │               │
   │                  │               │                 │──gRPC request───────────────────►
   │                  │               │                 │                 │               │
   │                  │               │                 │◄──SetResponse────────────────── │
   │                  │               │                 │   (discarded)   │               │
   │  (no response)   │               │                 │                 │               │
```
