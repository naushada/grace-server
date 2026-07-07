# tNMI mgmt dial-out

A bidirectional management channel for devices behind NAT. A device dials in and
opens the bidi RPC **`/tnmi.DialTcc/Subscribe`**:

```
device ──dials──► server :58989
        Subscribe(stream DeviceResponse) returns (stream DeviceRequest)
   operator ──DeviceRequest (command) ──► device        (down)
   device   ──DeviceResponse (result / proactive push)─► operator  (up)
```

You type a request in the console; it is packed into a `DeviceRequest` with a
random `rpc_id` and sent to every connected device. Each `DeviceResponse` is
correlated back to its command by `rpc_id` — an unmatched one is an unsolicited
(proactive) push; a `fake=true` heartbeat is ignored.

Supported request types (packed into `DeviceRequest.request`):
- **CLI** — `CliRequest{cmd, args, cec_cli, json, timeout}`
- **gNMI** — `gnmi.GetRequest` / `SetRequest` / `SubscribeRequest`

(The vendored proto is a CLI+gNMI-focused subset of upstream `tnmi_dialout.proto`
at `app/idl/dialout/tnmi_dialout.proto`, package `tnmi`.)

## Run it

Easiest — the wrapper (builds `marvel:dev` if needed, single-container TUI):
```bash
./service.sh mgmt                          # command TUI on :58989
./service.sh mgmt --out-file ./logs/mgmt.txt   # + save every response to a file
```
Or directly:
```bash
docker run -it --rm -p 58989:58989 marvel:dev /app/app --mode=mgmt-dialout
#   --tls=true --cert/--key/--ca   TLS
#   --log-file=<path>              append every response to a file
#   --headless=true                stdout instead of the TUI
```
Wait for a device to dial in — the sessions pane shows `#1`, and the transcript
logs `[mgmt] session #1 opened`.

## Sending requests (the input line)

| Type this | Sends |
|---|---|
| `show version` | CLI `show version` on the BN |
| `@RN-147 show version` | CLI on device **RN-147** (inline `@<device_id>`) |
| `gnmi get /system/state,/interfaces` | gNMI Get |
| `gnmi set /a/b/config/enabled:true` | gNMI Set |
| `gnmi subscribe /interfaces 10s` | gNMI Subscribe (SAMPLE every 10s; omit ⇒ on-change) |
| `@RN-147 gnmi get /system/state` | gNMI Get on RN-147 |

`@<device_id>` is **inline, per command** (omit ⇒ the BN). A random `rpc_id` is
generated per request; the echo shows it: `[mgmt] → @RN-147 'show'  rpc=r-8f3a…`.

### Sticky CLI settings (`:set`)

CLI-only knobs that apply to **every following** command until changed (shown in
the header):
```
:set cec on            # prefix cec_cli
:set json on           # append --json (cec)
:set timeout 20s       # CliRequest.timeout (10s / 500ms / 1m / 2h …)
:show                  # list current settings
:reset                 # clear them
```
Example: `:set cec on` then `show interfaces` → runs `cec_cli show interfaces`.

`quit` / `exit` / `Ctrl-D` leaves. `PgUp/PgDn/Home/End` scroll the transcript.

## Reading responses

Responses stream into the transcript, colour-coded:
- **green** `[mgmt] reply '<cmd>'  rpc=…` — a reply to a command you sent
- **magenta** `[mgmt] push …` — an unsolicited/proactive push from the device
- CLI result → `    exit=<n>  <ms>ms` then stdout (and `[stderr] …`)
- gNMI Get → `    /path = value` per leaf
- gNMI Subscribe → one JSON line per `SubscribeResponse` (streams over time)
- gNMI Set → `    set OK, <n> result(s)`

`--out-file` / `--log-file` appends every one of these lines to a host file
(works with the TUI or `--headless`), so you get a full transcript on disk.

## Notes

- CLI + gNMI ride the **same** dial-out; the grpc-tunnel byte-proxy (`:9339`)
  remains a separate, native-h2 gNMI path (see `grpc-tunnel-server.md`).
- The same server also serves `DialTcc.IsAlive` and `PushSubscriptionUpdates`
  (telemetry) on the dial-in connection.
- Omitted from the vendored proto vs upstream: gNOI request types,
  `DeviceResponse.status` (skipped on parse), and the `DialTarget`/RegisterDialout
  control plane — add those deps for full parity when needed.
