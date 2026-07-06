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

**Increment B (done): Tunnel data plane.** A local gNMI listener (`--local-port`,
`--target`) byte-proxies operator connections to a registered target: on accept
the server sends `Session{tag}` on the target's Register stream; the device opens
`Tunnel(stream Data)`; the first `Data{tag}` pairs the stream and bytes relay
both ways (operator socket ⇄ `Data{tag}` ⇄ device's local gNMI). The listener is
raw TCP, so operator-side TLS (if any) is tunnelled end-to-end to the device.

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
# or plain / headless, with the data-plane listener enabled:
docker run -d --name tunnel -p 58989:58989 -p 9339:9339 \
  gnmiserver:dev /app/app --mode=grpc-tunnel-server --port=58989 --headless=true \
    --local-port=9339 --target=dev1
# TLS: add --tls=true --cert=… --key=…  (no --ca ⇒ one-way TLS, no client cert)
```

With `--local-port=9339 --target=dev1`, point a gNMI client at the server's
`:9339` and its traffic is byte-proxied to `dev1`'s local gNMI over the tunnel.

Headless logs (also shown in the TUI transcript):
```
[reg] Register stream opened (stream=…)
[reg] +target 'dev1' (GNMI_GNOI) — 1 total
```

## Smoke test (no device)

`docs/tunnel-smoke.sh` uses grpcurl to open `Register` and send
`Target{op:ADD, target_type:GNMI_GNOI}`, asserting the server acks and logs the
target. See that script's header for usage.
