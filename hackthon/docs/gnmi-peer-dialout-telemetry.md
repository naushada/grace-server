# gRPC dial-out streaming telemetry ↔ gNMI peer (troubleshooting runbook)

How a device that **dials out** over gRPC/HTTP2 connects to the grace-server
`gnmi_peer` container and pushes streaming telemetry, why such an integration
fails layer by layer, and how each layer is handled.

This doc doubles as a **layered troubleshooting guide**: getting telemetry
flowing requires success at seven layers, and the failure looks different at
each one. Work from the bottom up.

| # | Layer | Failure symptom | Fix |
|---|---|---|---|
| 1 | IP reachability | no SYN at host | firewall / routing; `local=0.0.0.0` |
| 2 | TLS / h2c framing | `-903` bad client magic | match `tls.enabled` both ends |
| 3 | Listener up | TCP `RST` / refused | container up + published; run `-d` |
| 4 | gRPC service | `UNIMPLEMENTED` (12) | register the RPC paths the device calls |
| 5 | Compression | garbage / parse fail | inflate gzip gRPC frames |
| 6 | Streaming shape | data arrives, nothing dispatched | client-streaming dispatch (no END_STREAM) |
| 7 | Decode | opaque bytes | parse as `gnmi.SubscribeResponse` |

### The dial-out pattern

Some devices don't wait to be polled — they **dial out** to a collector and
stream telemetry over a persistent gRPC/HTTP2 connection. A common shape:

1. The device opens a connection to the peer's listener.
2. It calls a **liveness RPC** (unary) to confirm the collector is up.
3. It opens a **client-streaming push RPC** and streams `gnmi.SubscribeResponse`
   messages continuously (no half-close).

The peer must register handlers for the exact RPC paths the device uses. Those
registrations live in `app/src/client_app.cpp` (`register_gnmi_handlers`, run per
accepted connection). Because the push carries **standard
`gnmi.SubscribeResponse`** — a type grace-server already compiles — telemetry
needs no vendor proto to decode.

---

## Topology

```
  Device (LAN)                                Docker host (Linux)
  e.g. 192.168.100.2                          e.g. 10.0.60.48
        │                                            │
        │   gRPC/HTTP2 dial-out  ── TCP :58989 ──►   │  -p 58989:58989
        │   liveness RPC, then telemetry push        │        │
        │                                            │        ▼
        │                                       ┌─────────────────────────┐
        │                                       │ gnmi_peer container      │
        │                                       │  local  0.0.0.0:58989    │  ← listens
        │                                       │  remote <device>:<port>  │  ← dial-out only
        │                                       └─────────────────────────┘
```

Direction that matters here: **the device dials INTO the peer** and pushes. The
peer is passive (listens on `local`). `remote` is only used if the peer itself
issues `gnmi get/set/subscribe`.

### The IP rules (the part that trips everyone up)

| Field | Value | Why |
|---|---|---|
| peer `local.endpoint.ip` | **`0.0.0.0`** — always | binds all interfaces in the container; a specific/host IP silently fails to bind |
| peer `remote.endpoint.ip` | the **device's** LAN IP | only used when the peer dials out |
| device's target (device-side config) | the **Docker host's** LAN IP | how the device reaches the published port |

`host.docker.internal` is a Docker-Desktop-only alias for the host and does
**not** resolve on native Linux Docker. On the LAN you use real IPs.

---

## The layers (and how each failed)

### 1. IP reachability — "is the packet reaching :58989?"
Symptom: nothing arrives. Trace it hop by hop with `tcpdump`:
```bash
sudo tcpdump -ni any 'tcp port 58989'          # on the Docker host
```
- No SYN at the host → LAN/routing/firewall before Docker (`ufw`, `firewalld`,
  `iptables`). Open it: `sudo ufw allow 58989/tcp`.
- SYN arrives → continue to layer 2.

### 2. TLS / h2c framing — error `-903`
Symptom (peer log): `grpc/http2 recv error: -903`, connection torn down, looping.
`-903` = nghttp2 `NGHTTP2_ERR_BAD_CLIENT_MAGIC` — the client's first bytes were
**not** a valid HTTP/2 preface. Causes:
- The client is speaking **TLS** while the server is plaintext (or vice-versa).
  A TLS ClientHello starts `16 03 …`; the h2c preface is `PRI * HTTP/2.0`.
  **`tls.enabled` must match on both ends.** (Note: many gRPC tools' `--insecure`
  means *TLS without verification*, still TLS.)
- Benign noise: your own `nc`/`curl`/browser probes also log `-903`.

Identify with the first bytes on the wire:
```bash
sudo tcpdump -ni any 'tcp port 58989' -A -c 20
```

### 3. Listener up — TCP `RST` / connection refused
Symptom (device tcpdump): SYN answered immediately by `Flags [R.]` (RST).
Nothing is listening on the host:port at connect time. Causes:
- Container not running. **Gotcha:** running headless with `--rm -i` on a bare
  terminal exits on stdin-EOF, and `--rm` then deletes the container → RST loop.
  Fix: run **detached** (`-d`).
- Wrong host IP (device aimed at a box with nothing on 58989).
- Publish bound to loopback (`127.0.0.1:58989`) instead of `0.0.0.0`.

Check on the host:
```bash
docker ps                       # want 0.0.0.0:58989->58989/tcp
sudo ss -ltnp | grep 58989
ip -4 addr | grep <target-ip>   # confirm the device is aimed at THIS host
```

### 4. gRPC service — `UNIMPLEMENTED` (status 12)
Once layers 1–3 are green the HTTP/2 + gRPC connection establishes and data
flows. But the device calls its own service RPCs — a liveness probe and a
telemetry-push method — which grace-server may not register. The peer serves
only registered paths; an unknown path returns **gRPC status 12 (UNIMPLEMENTED)**
(`grpc_session.cpp` unknown-method branch). The device's liveness check then
fails, so it concludes the collector is dead and never streams — an endless
retry loop.

Find the exact paths the device uses (from the peer's own logs, once built with
the diagnostics below, or on the wire):
```bash
sudo tcpdump -ni any 'tcp port 58989' -A | grep -a ':path'
```
Then register a handler for each in `client_app.cpp`.

### 5. Compression — gzip gRPC frames
The telemetry stream may set `grpc-encoding: gzip`. Each gRPC message frame is
`[1-byte Compressed-Flag][4-byte big-endian length][body]`; when the flag is `1`
the body is gzip'd. The frame decoder ignored the flag and handed raw gzip to the
parser → garbage. Fix: `grpc_session::decode_frame` inflates (zlib, linked via
`gnmi_proto` PUBLIC so every target gets it).

### 6. Streaming shape — client-streaming, no END_STREAM
A telemetry push is typically **client-streaming**: the device opens the stream
and pushes a message periodically but **never sends END_STREAM**. Dispatch fired
only on END_STREAM (`http2.cpp on_frame_recv`), so messages were buffered into
`req.body` and never handed to a handler — data on the wire, silence in the logs,
and `req.body` growing without bound.
Fix: `http2_session::set_request_stream_handler` fires per DATA frame;
`grpc_session::register_client_stream` + `on_request_stream` decode and dispatch
each complete message as it arrives (consuming it from the buffer).

### 7. Decode — parse as `gnmi.SubscribeResponse`
The streamed messages are `gnmi.SubscribeResponse`, so `client_app.cpp` parses
each and renders it — no vendor proto required.

---

## The handlers

Registered in `app/src/client_app.cpp` (`register_gnmi_handlers`):

**Liveness (unary)** — answer OK with an empty body. The unary handler API works
on raw protobuf bytes, so no vendor proto is needed; an empty message is a valid
proto3 response for any type and `grpc-status: 0` is what a liveness probe checks.

```cpp
m_grpc->register_unary(
    "<liveness RPC path>",
    [](const std::string &req_pb) -> std::pair<int, std::string> {
      return {0, ""};   // grpc-status 0, empty message
    });
```

**Telemetry (client-streaming)** — decode each message as `gnmi.SubscribeResponse`
and emit one readable line per leaf:

```cpp
m_grpc->register_client_stream(
    "<telemetry push RPC path>",
    [](std::int32_t sid, const std::string &msg_pb) {
      gnmi::SubscribeResponse resp;
      if (!resp.ParseFromString(msg_pb)) { /* parse fail */ return; }
      if (resp.response_case() == gnmi::SubscribeResponse::kSyncResponse) {
        update_sink::instance().emit("── sync ──"); return;
      }
      const gnmi::Notification &n = resp.update();
      const std::string prefix = gnmi_util::path_to_string(n.prefix());
      update_sink::instance().emit("── " + format_ns_timestamp(n.timestamp()) +
                                   " · " + std::to_string(n.update_size()) +
                                   " update(s) ──");
      for (const auto &u : n.update())
        update_sink::instance().emit(prefix + gnmi_util::path_to_string(u.path()) +
                                     " = " + gnmi_util::typed_value_to_json(u.val()));
    });
```

Output (one leaf per line — readable, greppable):
```
[remote] ── 2026-07-05T18:19:25.145Z · 42 update(s) ──
[remote] /system/software/state/boot-reason = "warm boot"
[remote] /system/state/uptime = 217218
```

The notification `timestamp` (int64 nanoseconds) is shown once per notification
(shared by all its leaves), formatted as UTC with millisecond precision.

---

## Deploy

Rebuild and run detached on the Docker host:
```bash
cd hackthon
./build.sh -t gnmiserver:dev

# Option A — the helper script (generates endpoint.lua + runs detached):
REMOTE_IP=<device-LAN-ip> docs/run-gnmi-peer.sh

# Option B — plain docker:
docker run -d --name gnmi_peer -p 58989:58989 \
  -v "$PWD/endpoint.lua:/app/command/endpoint.lua:ro" \
  gnmiserver:dev /app/gnmi_peer --config=/app/command/endpoint.lua --headless=true
```

Verify (headless):
```bash
docker logs -f gnmi_peer
```
- `[remote] <path> = <value>` lines → decoded telemetry is arriving. Done.

Diagnostics baked in for future issues:
- `[grpc] UNIMPLEMENTED <path>` — an RPC the peer doesn't serve (name the method
  to add a handler).
- `[grpc] gzip inflate failed (…)` — a compressed frame that wouldn't inflate.

## Interactive TUI

Run without `--headless` (needs a TTY) for the two-pane shell:
```bash
docker run --rm -it -p <host-lan-ip>:58989:58989 \
  -v "$PWD/endpoint.lua:/app/command/endpoint.lua:ro" \
  gnmiserver:dev /app/gnmi_peer --config=/app/command/endpoint.lua
```

- **Readable telemetry** in the scrolling transcript (one `path = value` per line).
- **Scrollback + scrollbar**: PgUp/PgDn (page), ↑/↓ (line), Home (oldest),
  End (newest / resume live-follow). Scrolling up holds the view while new
  telemetry streams in. Needed because, under tmux, the ncurses alternate screen
  hides the transcript from tmux's own scrollback — so to find a one-shot marker
  like `── sync ──`, PgUp within the TUI.
- **Resize-aware**: the layout reflows on terminal resize (SIGWINCH), handled as
  a libevent signal event since input is stdin-driven.

## Saving updates to a file

`--out=<file>` appends every received update (the `[remote]` telemetry) to a
file, in addition to displaying it — flushed per line so it is tail-able live.
Especially useful in TUI mode, where stdout is the ncurses display and cannot be
redirected.

```bash
# via the helper (mounts the host file, chmods it for the container user):
OUT=./updates.log REMOTE_IP=<device-ip> docs/run-gnmi-peer.sh
tail -f ./updates.log

# or plain docker (mount a host file, pass --out to the container path):
touch ./updates.log && chmod 666 ./updates.log
docker run -d --name gnmi_peer -p 58989:58989 \
  -v "$PWD/endpoint.lua:/app/command/endpoint.lua:ro" \
  -v "$PWD/updates.log:/app/updates.log" \
  gnmiserver:dev /app/gnmi_peer --config=/app/command/endpoint.lua \
    --headless=true --out=/app/updates.log
```

The file receives the same one-line-per-leaf format (with `── timestamp ──`
headers), minus the `[remote]` prefix — clean for grep/analysis.
