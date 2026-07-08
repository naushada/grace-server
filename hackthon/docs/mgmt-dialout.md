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

## Sequence flow

```mermaid
sequenceDiagram
    autonumber
    participant Op as Operator<br/>(mgmt TUI, in-process)
    participant Srv as mgmt server<br/>:58989
    participant Dev as Device<br/>(DialTcc client)

    Note over Dev,Srv: TCP #1 (HTTP/2) — device dials OUT
    Dev->>Srv: opens DialTcc.Subscribe (bidi stream)
    Srv->>Dev: DeviceRequest{rpc_id=probe, gnmi Get /system/state}
    Dev-->>Srv: DeviceResponse{rpc_id=probe, GetResponse}
    Note over Op,Srv: prompt → role(hostname)>, banner printed

    Op->>Srv: types "cec_cli connections_show"
    Srv->>Dev: DeviceRequest{rpc_id=r-8f3a, CliRequest}
    Dev-->>Srv: DeviceResponse{rpc_id=r-8f3a, CliResponse}
    Note over Op: green reply (correlated by rpc_id)

    Dev-->>Srv: DeviceResponse{rpc_id="", unsolicited}
    Note over Op: magenta proactive push (anytime)
```

**TCP/streams:** a single device dial-out socket (`:58989`, HTTP/2) carries **one
long-lived `DialTcc.Subscribe` stream** that carries *all* commands, results, and
proactive pushes (plus `IsAlive` / `PushSubscriptionUpdates`). There is **no
operator socket** — the mgmt TUI runs inside the server process. Full breakdown:
[dialout-overview.md](dialout-overview.md#tcp-sockets--streams).

## Run it

Easiest — the wrapper (builds `marvel:dev` if needed, single-container TUI). It
mounts `docs/mgmt-requests` at `/req` and forces `TERM=xterm-256color` (so keys
work under tmux):
```bash
./service.sh mgmt                              # command TUI on :58989
./service.sh mgmt --out-file ./logs/mgmt.txt   # + save every response to a file
./service.sh mgmt --req-dir ./my-requests      # mount a request-.lua dir at /req
```
Or directly:
```bash
docker run -it --rm -e TERM=xterm-256color -p 58989:58989 \
  marvel:dev /app/app --mode=mgmt-dialout
#   --tls=true --cert/--key/--ca   TLS
#   --log-file=<path>              append every response to a file
#   --headless=true                stdout instead of the TUI
```
Under tmux, force `TERM=xterm-256color` as shown (tmux's `screen`/`tmux-256color`
may be missing from the container's terminfo, which breaks PgUp/Home/wheel).

Wait for a device to dial in — the sessions pane shows `#1`, and the transcript
logs `[mgmt] session #1 opened`.

## The console (TUI)

A full-height transcript with a compact bottom (session line + input box):
```
 Marvel gNMI Mgmt · :58989 · 1 session(s)      PgUp/PgDn·End scroll · ^D quit  ← header
 ▸ bn(S147F2223907369) connected · device S147F2223907369                      ← transcript
 [mgmt] → gnmi get /system/state  rpc=r-8f3a…                                     (fills the
     /system/state/hostname = "bn-1"  …                                          whole area)
 #1 bn(S147F2223907369)  3m      · defaults                                    ← session + settings
 ╭────────────────────────────────────────────╮
 │ bn(S147F2223907369)> gnmi get /system/state │                               ← Claude-style box
 ╰────────────────────────────────────────────╯
```
Same UI family as the other TUIs: the grpc-tunnel monitor colours/chrome plus the
gnmi_peer's **Claude-style rounded input box**. There's no top pane — the
transcript fills the height; the session summary (`#id role(hostname) uptime`)
and sticky settings sit in the footer next to the box.

- On session-open the server auto-probes `gnmi Get /system/state` and prints a
  **banner** (`▸ role(hostname) connected …`); the **prompt** becomes
  `role(hostname)> `.
- **Keys:** `Up`/`Down` recall command history; scroll vertically with
  **`Shift+↑`/`Shift+↓` (one line)**, `PgUp`/`PgDn` (page), `Home`/`End`
  (top/bottom), or the mouse wheel; **`←`/`→` scroll horizontally** for wide
  output (a bottom h-scrollbar shows the position); `help` (or `?`) lists
  commands; `quit`/`exit`/`^D` leaves.
- Colours: cyan `[mgmt]` headers, green replies, magenta proactive pushes, yellow
  errors.

> **Keys under tmux:** the console runs with `TERM=xterm-256color`, so `PgUp`/`PgDn`,
> `Home`/`End`, `←`/`→` (horizontal scroll) and `↑`/`↓` (history) pass straight
> through. Two gotchas: **(1)** if the top-right shows `[N/M]` you're in tmux
> **copy-mode** — press `q` to exit, otherwise the arrows scroll tmux, not the
> console; **(2)** tap the arrow directly — pressing the tmux **prefix** (`Ctrl-B`)
> first turns it into a tmux command. `Shift+↑`/`Shift+↓` (one-line scroll) also
> need `set -g xterm-keys on` in `~/.tmux.conf` (default on in tmux ≥ 2.4); the
> plain arrows don't.

## Sending requests (the input line)

| Type this | Sends |
|---|---|
| `show version` | CLI `show version` on the BN |
| `:set cec_cli on` then `connections_show` | `cec_cli connections_show` (cec_cli prefix; add `:set json on` for `--json`) |
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
:set cec_cli on            # prefix cec_cli
:set json on           # append --json (cec)
:set timeout 20s       # CliRequest.timeout (10s / 500ms / 1m / 2h …)
:show                  # list current settings
:reset                 # clear them
```
Example: `:set cec_cli on` then `show interfaces` → runs `cec_cli show interfaces`.

`quit` / `exit` / `Ctrl-D` leaves. `PgUp/PgDn/Home/End` scroll the transcript.

## Request files (Lua → proto)

For canned or complex requests, describe the whole `DeviceRequest` in a `.lua`
file and send it with **`send <file.lua>`** — the table is serialized straight
into the proto by reflection (no per-type code). `rpc_id` is stamped for you.

Rules:
- The top table **is** the `tnmi.DeviceRequest` (`device_id` + `request`).
- The `request` field is a `google.protobuf.Any` — name its message with
  **`["@type"] = "<proto full name>"`** (e.g. `gnmi.GetRequest`,
  `tnmi.DeviceRequest.CliRequest`); it's built, populated, and packed.
- Scalars, arrays, nested tables, and arrays-of-tables map to scalar / repeated /
  nested-message / repeated-message fields. **Enums** are given by name
  (`encoding = "JSON"`). **Maps** (e.g. `PathElem.key`) are a nested `k = v` table.
- **Path sugar**: a string assigned to a `gnmi.Path` field is parsed as a YANG
  path, so **keys** work inline: `"/interfaces/interface[name=eth0]/state"`.

```lua
-- gnmi_get.lua  →  send docs/mgmt-requests/gnmi_get.lua
return {
  device_id = "RN-147",
  request = {
    ["@type"] = "gnmi.GetRequest",
    encoding  = "JSON",
    path = { "/system/state", "/interfaces/interface[name=eth0]/state" },
  },
}
```
Samples in **`docs/mgmt-requests/`**: `cli.lua`, `cec_connections.lua`
(cec_cli command), `gnmi_get.lua`, `gnmi_get_key.lua` (explicit key map),
`gnmi_set.lua` (TypedValue oneof), `gnmi_subscribe.lua`.

`./service.sh mgmt` auto-mounts `docs/mgmt-requests` at **`/req`** (override with
`--req-dir <dir>`), so `send /req/gnmi_get.lua` works out of the box. With a bare
`docker run`, mount it yourself:
```bash
docker run -it --rm -p 58989:58989 -v "$PWD/docs/mgmt-requests:/req:ro" \
  marvel:dev /app/app --mode=mgmt-dialout
#   in the TUI:  send /req/gnmi_get.lua
```
(`.lua` files are read from the container filesystem, so the path you `send` is
the in-container path — e.g. `/req/…`.)

## Reading responses

Responses stream into the transcript, colour-coded:
- **green** `[mgmt] reply '<cmd>'  rpc=…` — a reply to a command you sent
- **magenta** `[mgmt] push …` — an unsolicited/proactive push from the device
- CLI result → `    exit=<n>  <ms>ms` then stdout (and `[stderr] …`)
- gNMI Get → `    /path = value` per leaf (cyan)
- gNMI Subscribe → `    [notif #N, updates:M] <json>` per notification (magenta) —
  `N` is the running notification count, `M` the leaves it carries (`, del:D` when
  it deletes paths)
- gNMI Set → `    set OK, <n> result(s)` (green)

`--out-file` / `--log-file` appends every one of these lines to a host file
(works with the TUI or `--headless`), so you get a full transcript on disk.

## Also serves the gNMI tunnel (:9339)

The device dials `:58989` **once** and opens *both* `DialTcc.Subscribe` (this
console) **and** grpctunnel `Register`/`Tunnel` on that single bidi connection. So
`service.sh mgmt` also brings up the **tunnel data-plane on `:9339`** — external
gNMI clients (gnmic, gnmi_peer) reach the device byte-for-byte over the same
dial-out, while the console runs:

```bash
./service.sh mgmt                 # console on :58989 + gNMI tunnel on :9339
# from anywhere:
gnmic -a <host>:9339 --insecure get --path /system/state
```

It mounts `docs/tunnel.lua` (the same port→target map as `grpc-tunnel`) and
publishes `:9339`. When the tunnel is active here, the console shows the
`[reg] +target …` line and the map hint (edit `docs/tunnel.lua`, `./service.sh
restart`) — exactly like the tunnel runbook. Directly:
```bash
docker run -it --rm -p 58989:58989 -p 9339:9339 \
  -v "$PWD/docs/tunnel.lua:/app/tunnel.lua:ro" \
  marvel:dev /app/app --mode=mgmt-dialout --config=/app/tunnel.lua
#   or a single listener:  --local-port=9339 --target='<device published target>'
```

## Notes

- Two ways to reach the device's gNMI from one dial-out: **wrapped** in a
  `DeviceRequest` over `DialTcc` (in this console), or **transparent** over the
  `:9339` byte-proxy (any external gNMI client). See `grpc-tunnel-server.md` for
  the tunnel details and `dialout-overview.md` for the socket/stream model.
- The same server also serves `DialTcc.IsAlive` and `PushSubscriptionUpdates`
  (telemetry) on the dial-in connection.
- Omitted from the vendored proto vs upstream: gNOI request types,
  `DeviceResponse.status` (skipped on parse), and the `DialTarget`/RegisterDialout
  control plane — add those deps for full parity when needed.
