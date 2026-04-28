# gNMI over MQTT–VPN Flow

This document describes how a gNMI operation typed at the CLI reaches a gNMI
server that sits behind an OpenVPN tunnel, and how the response travels back.

---

## Architecture Overview

Five long-lived processes must be running simultaneously:

| Process | Binary / mode | Role |
|---------|--------------|------|
| **CLI** | `cli_app` | readline REPL; builds gNMI proto, publishes request, waits for response |
| **Relay** | `grace-server --mode=gnmi-mqtt-client` | bridges `cli/#` → `fwd/#` and `resp/#` → `cli_resp/#` |
| **VPN server** | `grace-server --mode=server --mqtt-host=...` | subscribes `fwd/#`, injects request into VPN tunnel, publishes response to `resp/<vip>` |
| **VPN client** | `grace-server --mode=client` | receives framed request; nftables DNAT → local gNMI server |
| **gNMI server** | `grace-server --mode=gnmi-server` | handles the RPC, returns SetResponse / GetResponse |

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
│         a. subscribe to "cli_resp/<vip>"  (before publish)          │
│         b. publish to  "cli/<vip>"                                  │
│            payload = "/gnmi.gNMI/Set\0<proto_bytes>"                │
│         c. poll mosquitto_loop() up to 5 s for response             │
└───────────────────┬─────────────────────────────────────────────────┘
                    │  MQTT broker
                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│ Relay  (--mode=gnmi-mqtt-client)         main_app.cpp               │
│                                                                     │
│  mqtt_io relay(..., relay_msg_cb)                                   │
│  relay.subscribe("cli/#")                                           │
│  relay.subscribe("resp/#")                                          │
│                                                                     │
│  relay_msg_cb:                                                      │
│    "cli/<ip>"  → publish "fwd/<ip>"       (request direction)       │
│    "resp/<ip>" → publish "cli_resp/<ip>"  (response direction)      │
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
│    gnmi_client::push_async(dest_ip, gnmi_port, rpc_path,            │
│                            proto_bytes, {}, response_lambda)        │
│      → opens TCP to <vip>:58989 through VPN tunnel                  │
│      → sends HTTP/2 POST /<rpc_path> with gRPC-framed proto         │
└───────────────────┬─────────────────────────────────────────────────┘
                    │  OpenVPN tunnel  (tun0 ↔ tun0)
                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│ VPN client  (--mode=client)              openvpn_client.cpp         │
│                                                                     │
│  Receives TCP traffic destined for <vip>:58989 on tun0.             │
│  nftables DNAT: <vip>:58989 → 127.0.0.1:58989                       │
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

## Response Flow (gNMI server → CLI)

```
┌─────────────────────────────────────────────────────────────────────┐
│ gNMI server                                                         │
│  Sends SetResponse / GetResponse over HTTP/2 connection.            │
└───────────────────┬─────────────────────────────────────────────────┘
                    │  HTTP/2 over VPN tunnel
                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│ VPN server  — gnmi_connection::capture_response()                   │
│                                                                     │
│  finish() called → response_lambda fires:                           │
│    payload = rpc_path '\0' status '\0' grpc_message '\0' proto_bytes│
│    m_mqtt_io->publish("resp/<vip>", payload)                        │
└───────────────────┬─────────────────────────────────────────────────┘
                    │  MQTT broker
                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│ Relay                                                               │
│  Receives "resp/<vip>", republishes to "cli_resp/<vip>"             │
└───────────────────┬─────────────────────────────────────────────────┘
                    │  MQTT broker
                    ▼
┌─────────────────────────────────────────────────────────────────────┐
│ CLI  — on_cli_response() callback                                   │
│                                                                     │
│  Parses: rpc_path '\0' status '\0' grpc_message '\0' proto_bytes    │
│  print_mqtt_response():                                             │
│    status == 0  → deserialise SetResponse/GetResponse, text-format  │
│    status != 0  → print gRPC error status + message                 │
│    status < 0   → print transport error (timeout, closed, etc.)     │
└─────────────────────────────────────────────────────────────────────┘
```

---

## Payload Wire Formats

### Request  (`cli/<vip>` and `fwd/<vip>`)

```
[ rpc_path ][ 0x00 ][ proto_bytes ]
```

| Field | Example | Notes |
|-------|---------|-------|
| `rpc_path` | `/gnmi.gNMI/Set` | ASCII string, null-terminated |
| `0x00` | — | Separator |
| `proto_bytes` | `<binary>` | Serialised SetRequest / GetRequest |

### Response  (`resp/<vip>` and `cli_resp/<vip>`)

```
[ rpc_path ][ 0x00 ][ status ][ 0x00 ][ grpc_message ][ 0x00 ][ proto_bytes ]
```

| Field | Example | Notes |
|-------|---------|-------|
| `rpc_path` | `/gnmi.gNMI/Set` | Echo of the request RPC path |
| `0x00` | — | Separator |
| `status` | `0` | gRPC status code as decimal ASCII; `-1` = transport error |
| `0x00` | — | Separator |
| `grpc_message` | `""` or `"permission denied"` | gRPC error message, may be empty |
| `0x00` | — | Separator |
| `proto_bytes` | `<binary>` | Serialised SetResponse / GetResponse (may contain `0x00`) |

Only the first three `0x00` bytes are used as separators; `proto_bytes` is the
remainder of the payload and passes through binary-safe.

---

## MQTT Topic Scheme

| Direction | Topic | Publisher | Subscriber |
|-----------|-------|-----------|------------|
| CLI → relay | `cli/<vip>` | `cli_app` (g_mosq) | relay (mqtt_io) |
| relay → VPN server | `fwd/<vip>` | relay (mqtt_io) | VPN server (m_mqtt_io) |
| VPN server → relay | `resp/<vip>` | VPN server (m_mqtt_io) | relay (mqtt_io) |
| relay → CLI | `cli_resp/<vip>` | relay (mqtt_io) | CLI (g_mosq) |

`<vip>` is the **virtual IP** assigned to the target VPN client (e.g. `10.8.0.2`).
It acts as the routing key across all four hops.

---

## `gnmi_connection` Completion Paths

`finish()` is called exactly once per connection, guarded by `m_done`:

| Path | Trigger | `grpc_status` |
|------|---------|---------------|
| Success | `capture_response()` — trailing HEADERS with `grpc-status` | `0` (or server-set code) |
| Timeout | `handle_event()` — `BEV_EVENT_TIMEOUT` | `-1` |
| Connection closed | `handle_close()` — `BEV_EVENT_EOF/ERROR` before response | `-1` |
| Protocol error | `handle_read()` — nghttp2 returns negative | `-1` |

---

## gNMI Operations Supported via CLI

| CLI command | gNMI proto built | RPC path |
|-------------|-----------------|----------|
| `gnmi_get` | `gnmi::GetRequest` | `/gnmi.gNMI/Get` |
| `gnmi_update` | `gnmi::SetRequest` (update[]) | `/gnmi.gNMI/Set` |
| `gnmi_replace` | `gnmi::SetRequest` (replace[]) | `/gnmi.gNMI/Set` |
| `gnmi_delete` | `gnmi::SetRequest` (delete[]) | `/gnmi.gNMI/Set` |

### Example session

```
# MQTT_HOST and MQTT_PORT must be set before starting cli_app
Marvel> gnmi_update target=10.8.0.2 path=/interfaces/interface[name=eth0]/config value={"enabled":true}
[mqtt] published 47B proto → topic=cli/10.8.0.2
[gnmi_set] OK
timestamp: 1234567890

Marvel> gnmi_get target=10.8.0.2 path=/interfaces/interface[name=eth0]/state/oper-status
[mqtt] published 38B proto → topic=cli/10.8.0.2
[gnmi_get] OK
notification {
  update { path { elem { name: "oper-status" } } val { string_val: "UP" } }
}

# On error (gRPC or transport):
Marvel> gnmi_update target=10.8.0.99 path=/foo value=bar
[mqtt] published 22B proto → topic=cli/10.8.0.99
[gnmi] response timeout (5s)
```

---

## Full Sequence Diagram

```
cli_app        MQTT broker     relay          vpn-server      vpn-client     gnmi-server
   │                │             │                │               │               │
   │─subscribe──────►             │                │               │               │
   │  cli_resp/<vip>│             │                │               │               │
   │─publish────────►             │                │               │               │
   │  cli/<vip>     │             │                │               │               │
   │                │─deliver─────►                │               │               │
   │                │  cli/<vip>  │                │               │               │
   │                │◄─publish────│                │               │               │
   │                │  fwd/<vip>  │                │               │               │
   │                │─deliver───────────────────── ►               │               │
   │                │  fwd/<vip>  │                │               │               │
   │                │             │         push_async(<vip>, cb)  │               │
   │                │             │                │─TCP connect───────────────────►
   │                │             │                │  (VPN tunnel) │               │
   │                │             │                │─gRPC POST─────────────────────►
   │                │             │                │               │               │
   │                │             │                │◄──SetResponse─────────────────│
   │                │             │         finish() → cb fires    │               │
   │                │◄─publish────────────────────-│               │               │
   │                │  resp/<vip> │                │               │               │
   │                │─deliver─────►                │               │               │
   │                │  resp/<vip> │                │               │               │
   │                │◄─publish────│                │               │               │
   │                │ cli_resp/<vip>               │               │               │
   │◄─deliver───────│             │                │               │               │
   │  cli_resp/<vip>│             │                │               │               │
   │                │             │                │               │               │
   │ on_cli_response()            │                │               │               │
   │ print_mqtt_response()        │                │               │               │
   │─unsubscribe────►             │                │               │               │
   │  cli_resp/<vip>│             │                │               │               │
```
