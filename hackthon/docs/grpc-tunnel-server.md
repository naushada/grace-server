# gRPC tunnel server — reach devices behind NAT

`app --mode=grpc-tunnel-server` lets the server reach devices it **cannot dial
into**. The device dials OUT to the server and holds a persistent bidirectional
gRPC stream open; the server pushes requests DOWN that stream. Once the tunnel
is up, operator **gNMI Get/Set/Subscribe** ride over it to the device.

This is the inverse of a normal gNMI target: instead of the collector connecting
to the device, the device connects to the collector — the standard trick for
NAT/firewall traversal.

---

## Topology

```
  Device (behind NAT)                         Server (public / reachable)
        │                                            │
        │  1. dial out, open Session ──────────────► │  app --mode=grpc-tunnel-server
        │  2. send Register{target_id}               │        │
        │  ◄──────────── TunnelRequest ───────────── │        │  operator gNMI in:
        │  3. execute gNMI locally                   │        │   Get/Set/Subscribe
        │  ─────────────── TunnelResponse ─────────► │        ▼   (prefix.target = id)
        │                                       ┌──────────────────────────────┐
        │                                       │ tunnel_hub: id → session      │
        │                                       │ + request correlation by id   │
        │                                       └──────────────────────────────┘
```

`endpoint.lua` is **not** used — the tunnel server is CLI-flag configured, like
`--mode=gnmi-server`.

---

## The protocol (`app/idl/tunnel/tunnel.proto`)

Vendor-neutral. Payloads are opaque bytes so any RPC can be tunnelled; gNMI is
carried as serialised gNMI messages, correlated by `id`.

```proto
service Tunnel {
  rpc Session(stream TunnelResponse) returns (stream TunnelRequest);
}

message Register      { string target_id = 1; }
message TunnelRequest { uint64 id = 1; string method = 2; bytes payload = 3; }
message TunnelResponse {
  uint64 id = 1;
  oneof body { Register register = 2; bytes payload = 3; string error = 4; }
}
```

The device is the gRPC **client**: it opens `Session`, sends `Register` first,
then answers each `TunnelRequest` by running the RPC (`method` + `payload`)
against its local gNMI stack and streaming back `TunnelResponse` frames.

---

## How gNMI flows over the tunnel

The server also accepts operator gNMI on the same listener. When forwarding is
on (it is, in `grpc-tunnel-server` mode), a gNMI request selects its target via
`prefix.target`:

| Operator RPC | Over the tunnel | Reply |
|---|---|---|
| `Get` / `Set` (unary) | `TunnelRequest{id, method, payload=<GetRequest…>}` | one `TunnelResponse{id, payload=<GetResponse…>}` completes it |
| `Subscribe` (server-stream) | same, `method=/gnmi.gNMI/Subscribe` | every `TunnelResponse{id, payload=<SubscribeResponse>}` is relayed to the operator's stream |

Errors: no `prefix.target` → `INVALID_ARGUMENT (3)`; target not connected →
`UNAVAILABLE (14)`; target reports failure → `UNKNOWN (2)` / stream ends.

---

## Architecture

Three reusable pieces, built on the existing gRPC-over-nghttp2 stack:

1. **Bidi transport** — a dial-out client never half-closes, so the server
   can't wait for END_STREAM to start responding. `http2_session` fires a
   *headers-received* hook so `grpc_session::register_bidi_stream()` opens the
   response as soon as the stream opens; incoming frames reuse the
   client-streaming decode path, outgoing use the server-streaming send path.
2. **Async unary responses** — `grpc_session::register_unary_async()` hands the
   handler a `respond` callback to invoke when the reply arrives, so an
   operator's `Get` never blocks the event loop while the round-trip happens.
   `respond` self-guards via a shared alive-flag: a late reply after the
   operator disconnects is a safe no-op.
3. **`tunnel_hub`** — registry of live target sessions (by `target_id`) plus
   request correlation by `id`: unary (`add_pending`→`on_target_payload`
   completes) and streaming (`add_stream` relays each reply). A **target**
   disconnect fails/ends every request routed to it; an **operator** disconnect
   drops the relays it owns — no hung requests or streams.

---

## Run

```bash
cd hackthon
./build.sh -t gnmiserver:dev

# via run.sh:
./run.sh --image gnmiserver:dev grpc-tunnel-server
# or plain:
docker run -d --name tunnel -p 58989:58989 \
  gnmiserver:dev /app/app --mode=grpc-tunnel-server --port=58989
# add --tls=true --cert=… --key=… --ca=… for TLS.
```

Startup logs `[main] mode=grpc-tunnel-server port=58989 … (gNMI Get/Set
forwarded over tunnel)`, then per target:
```
[tunnel] session opened stream=…
[tunnel] target '<id>' registered (stream=…, N connected)
```
and per forwarded request:
```
[tunnel] -> '<id>' /gnmi.gNMI/Get id=7 (… req bytes)
```

An operator points a normal gNMI client at the server and sets
`prefix.target = <id>` to select the device.

---

## Known limits

- No **timeout** for a target that stays connected but never answers a request
  (cleanup fires on a disconnect, not on a hung/slow target).
- `Subscribe` **ONCE** mode has no explicit done-marker in `tunnel.proto`;
  `STREAM` mode (indefinite on-change telemetry — the normal case) works.
- The device implements `tunnel.proto` and drives the target side; only the
  server side lives here.
