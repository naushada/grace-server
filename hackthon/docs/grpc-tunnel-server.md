# gRPC tunnel server — openconfig/grpctunnel (reach devices behind NAT)

`app --mode=grpc-tunnel-server` is an **openconfig/grpctunnel** server. A device
behind NAT runs a grpctunnel **client**, dials OUT to this server, and Registers
the services it fronts (e.g. a `GNMI_GNOI` target). The server can then open a
data tunnel to that target and byte-proxy a connection to it.

grpctunnel is a **TCP-over-gRPC byte proxy** — it forwards raw bytes, it does not
parse gNMI. A gNMI session to a NATed device is tunnelled as an opaque byte
stream.

The proto is vendored verbatim at `app/idl/tunnel/tunnel.proto` (package
`grpctunnel`, version 0.2) — the wire format the device speaks.

---

## The protocol

```proto
service Tunnel {
  rpc Register(stream RegisterOp) returns (stream RegisterOp);  // control
  rpc Tunnel(stream Data)         returns (stream Data);        // data (bytes)
}
```

- **Register** — the client advertises `Target{op=ADD, target, target_type}`; the
  server acks (`accept=true`). To reach a target the server sends
  `Session{tag, target}`; both sides then open a **Tunnel** stream for that tag.
- **Tunnel** — `Data{tag, data, close}` carries the raw bytes of the proxied
  connection, multiplexed by tag.

---

## Topology

```
  Device (behind NAT)                         Server (reachable)
   grpctunnel client                          app --mode=grpc-tunnel-server
        │  Register ──► Target{ADD, GNMI_GNOI} │
        │  ◄── Target{accept}                  │   monitor TUI lists targets
        │                                      │
        │  (Increment B: Session{tag} ◄──────  │   ◄── operator connects
        │   Tunnel Data{tag} bytes ◄─────────► │       to a local gNMI listener
        │   → device's local gNMI :9339        │       bytes proxied over Tunnel
```

`endpoint.lua` is not used — CLI-flag configured, like `--mode=gnmi-server`.

---

## Status

**Increment A (done): Register.** The server accepts `Register`, records
`Target{ADD}`/drops `Target{REMOVE}`, acks, and lists live targets in a monitor
TUI. Run interactively for the dashboard; `--headless=true` for log-only.

**Increment B (todo): Tunnel data plane.** Session negotiation + `Tunnel` byte
streams + a local gNMI listener that byte-proxies an operator connection to a
target.

---

## Architecture (reused transport)

- **Bidi transport** — Register and Tunnel are both bidi. `http2_session` fires a
  headers-received hook so `grpc_session::register_bidi_stream()` opens the
  response as the stream opens (a dial-out client never half-closes); incoming
  frames reuse the client-streaming decode path, outgoing use the
  server-streaming send path.
- **`tunnel_hub`** — registry of registered targets (id → owning Register stream
  + type + since), cleaned up when a Register stream closes.
- **`tunnel_tui`** — the monitor: a targets pane (id / type / uptime) over a
  scrolling event transcript (scrollback + scrollbar + resize), 1s uptime tick.

---

## Run

```bash
cd hackthon
./build.sh -t gnmiserver:dev

# monitor TUI (interactive):
./run.sh --image gnmiserver:dev grpc-tunnel-server
# or plain / headless:
docker run -d --name tunnel -p 58989:58989 \
  gnmiserver:dev /app/app --mode=grpc-tunnel-server --port=58989 --headless=true
# TLS: add --tls=true --cert=… --key=…  (no --ca ⇒ one-way TLS, no client cert)
```

Headless logs (also shown in the TUI transcript):
```
[reg] Register stream opened (stream=…)
[reg] +target 'dev1' (GNMI_GNOI) — 1 total
```

## Smoke test (no device)

`docs/tunnel-smoke.sh` uses grpcurl to open `Register` and send
`Target{op:ADD, target_type:GNMI_GNOI}`, asserting the server acks and logs the
target. See that script's header for usage.
