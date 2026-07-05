# Tarana device ↔ gNMI peer integration (troubleshooting runbook)

How a Tarana radio/BN on the LAN connects to the grace-server `gnmi_peer`
container to push telemetry, why it initially failed, and how it was fixed.

This doc doubles as a **layered troubleshooting guide**: the connection has to
succeed at four layers, and the failure looks different at each one. Work from
the bottom up.

---

## Topology

```
  Tarana device (aarch64, LAN)                Docker host  (sjc-dev-02, Linux)
  e.g. 192.168.100.2 / 169.254.100.1          e.g. 10.0.60.48
        │                                            │
        │   gRPC/HTTP2 dial-out  ── TCP :58989 ──►   │  -p 58989:58989
        │   /tnmi.DialTcc/IsAlive (liveness)         │        │
        │   then telemetry push                      │        ▼
        │                                       ┌─────────────────────────┐
        │                                       │ gnmi_peer container      │
        │                                       │  local  0.0.0.0:58989    │  ← listens
        │                                       │  remote <device>:55555   │  ← dial-out only
        │                                       └─────────────────────────┘
```

Direction that matters here: **the device dials INTO the peer** and pushes.
The peer is passive (listens on `local`). `remote` is only used if the peer
itself issues `gnmi get/set/subscribe`.

### The IP rules (the part that trips everyone up)

| Field | Value | Why |
|---|---|---|
| peer `local.endpoint.ip` | **`0.0.0.0`** — always | binds all interfaces in the container; a specific/host IP silently fails to bind |
| peer `remote.endpoint.ip` | the **device's** LAN IP | only used when the peer dials out |
| device's target (device-side config) | the **Docker host's** LAN IP (`10.0.60.48`) | how the device reaches the published port |

`host.docker.internal` is a Docker-Desktop-only alias for the host and does
**not** resolve on native Linux Docker. On the LAN you use real IPs.

---

## The four layers (and how each failed)

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
  **`tls.enabled` must match on both ends.** (Note: many gNMI tools' `--insecure`
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

### 4. gRPC service — `UNIMPLEMENTED` (status 12)  ← the real root cause
Once layers 1–3 are green the HTTP/2 + gRPC connection establishes and data
flows. But the device calls:
```
:path /tnmi.DialTcc/IsAlive
content-type application/grpc
user-agent grpc-c++/1.75.1 grpc-c/50.0.0 (linux; chttp2)
```
`tnmi.DialTcc/IsAlive` is a **Tarana** liveness RPC — **not** a gNMI method.
grace-server only registered `gnmi.gNMI/{Capabilities,Get,Set,Subscribe}`, so
the unknown path returned **gRPC status 12 (UNIMPLEMENTED)**
(`grpc_session.cpp` unknown-method branch). The device's liveness check failed,
so it concluded the peer was dead and never streamed updates — an endless
IsAlive retry loop.

---

## The fix

Register a handler for `/tnmi.DialTcc/IsAlive` in
`app/src/client_app.cpp` (`register_gnmi_handlers`, runs per accepted
connection) that answers OK with an empty body:

```cpp
m_grpc->register_unary(
    "/tnmi.DialTcc/IsAlive",
    [](const std::string &req_pb) -> std::pair<int, std::string> {
      std::cout << "[IsAlive] DialTcc liveness probe (" << req_pb.size()
                << " req bytes) -> OK\n";
      return {0, ""};   // grpc-status 0, empty message
    });
```

The unary handler API works on **raw protobuf bytes**, so no `tnmi.DialTcc`
`.proto` is needed — an empty message is a valid proto3 response for any type,
and `grpc-status: 0` is what a liveness probe checks. Commit:
`feat: serve /tnmi.DialTcc/IsAlive so Tarana devices pass liveness`.

### Known follow-up
`IsAlive` only gets the device **past the liveness gate**. If, after that, it
calls other `tnmi.DialTcc` methods to actually stream telemetry, those still
return UNIMPLEMENTED. Capture the next path and implement it too:
```bash
sudo tcpdump -ni any 'tcp port 58989' -A | grep -a ':path'
```
For populated response fields, drop the real `tnmi.DialTcc` `.proto` into
`app/idl/` (wire it into `app/CMakeLists.txt` like the gNMI protos) and build
the response in the handler.

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

Verify:
```bash
docker logs -f gnmi_peer      # expect '[IsAlive] ... -> OK', then '[Set]' / '[remote]' lines
```
- `[IsAlive] ... -> OK` → the liveness gate passes (device tcpdump trailer flips
  from `grpc-status: 12` to `grpc-status: 0`).
- `[Set]` / `[remote]` lines → telemetry is arriving. Done.
