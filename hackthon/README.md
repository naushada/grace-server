# hackthon — gNMI gRPC Server over nghttp2

A C++20 gNMI target server built on libevent (I/O) + nghttp2 (HTTP/2) with a
hand-rolled gRPC framing layer.  No `libgrpc++` dependency — gRPC is
implemented directly over HTTP/2.

---

## Full Receive Pipeline

```
libevent (socket event)
  └─ client_read_cb()                     [app/src/framework.cpp]
       ├─ dry_run=true  → handle_read()   returns 0 ("can handle")
       └─ dry_run=false → handle_read()   [app/src/client_app.cpp]
            └─ grpc_session::recv()       [app/src/grpc_session.cpp]
                 └─ http2_session::recv() [app/src/http2.cpp  /  nghttp2]
                      └─ on_request()     fires when a complete HTTP/2 stream arrives
                           ├─ validates Content-Type: application/grpc[+proto]
                           ├─ grpc_session::decode_frame()   strips 5-byte length prefix
                           └─ registered unary handler  (keyed by URI path)
                                ├─ /gnmi.gNMI/Capabilities → CapabilityRequest  → CapabilityResponse
                                ├─ /gnmi.gNMI/Get          → GetRequest         → GetResponse
                                ├─ /gnmi.gNMI/Set          → SetRequest         → SetResponse
                                └─ /gnmi.gNMI/Subscribe    → UNIMPLEMENTED (grpc-status: 12)
                                     └─ send_unary_response()
                                          ├─ encode_frame()                wraps serialised proto bytes in 5-byte prefix
                                          ├─ http2_session::submit_response()  HEADERS (:status 200) + DATA (with trailer_mode)
                                          ├─ http2_session::submit_trailer()   trailing HEADERS (grpc-status)
                                          └─ raw_tx callback
                                               └─ connected_client::tx()   → libevent send buffer → socket
```

---

## gRPC Wire Format (implemented without libgrpc++)

| Layer       | What we do |
|-------------|------------|
| HTTP/2      | nghttp2 session (server-side); handles SETTINGS, WINDOW_UPDATE, PING, stream lifecycle |
| gRPC framing | 5-byte length-prefix: `[0x00][4-byte big-endian length][protobuf bytes]` |
| Trailers    | Trailing HEADERS frame carrying `grpc-status` (and optionally `grpc-message`) sent after DATA with `NO_END_STREAM` |
| Protobuf    | OpenConfig gNMI proto compiled by `protoc --cpp_out`; no `grpc_cpp_plugin` needed |

---

## Project Layout

```
app/
  inc/
    framework.hpp       libevent base, evt_io, server/client base classes
    http2.hpp           http2_session — transport-agnostic nghttp2 wrapper
    grpc_session.hpp    grpc_session  — gRPC framing + unary RPC dispatch
    client_app.hpp      connected_client — per-connection object
    server_app.hpp      server — accepts connections, owns connected_client map
    fs_app.hpp          filesystem watcher (inotify / Lua command loader)
    lua_engine.hpp      Lua 5.4 scripting engine
  src/
    http2.cpp
    grpc_session.cpp
    client_app.cpp      gNMI RPC handlers registered here
    server_app.cpp
    framework.cpp
    fs_app.cpp
    lua_engine.cpp
    main_app.cpp
  idl/
    gnmi/gnmi.proto
    gnmi_ext/gnmi_ext.proto
    collector/collector.proto
    target/target.proto
  test/
    http2_test.cpp
    grpc_session_test.cpp
    client_app_test.cpp
    server_app_test.cpp
    framework_test.cpp
    fs_app_test.cpp
    lua_engine_test.cpp
  cli/                  readline-based CLI (separate CMake target)
```

---

## Build

### Convenience scripts (`build.sh` / `run.sh`)

`hackthon/build.sh` and `hackthon/run.sh` wrap the container workflow and pick
**docker or podman** automatically (whichever is installed; override with
`--engine` or `ENGINE=`):

```bash
cd hackthon

./build.sh                       # build marvel:dev (no tests, fast)
./build.sh --tests on            # build and run the gtest suite
./build.sh -t marvel:dev --no-cache

./run.sh gnmi-peer               # interactive two-pane gNMI shell (port 58989)
./run.sh --config ./my.lua gnmi-peer
echo 'gnmi get /a/b' | ./run.sh --headless gnmi-peer
./run.sh --build gnmi-server -- --gnmi-port=9339   # build, then run
./run.sh shell                   # open a bash shell inside the image
./run.sh exec <name>             # bash into an already-running container
./run.sh --help                  # every command + option
```

`run.sh` covers every binary: `gnmi-peer`, `gnmi-server`, `cli`, `app`,
`vpn-server`, `vpn-client`, `openvpn-server`, `openvpn-client`, plus `shell`,
`exec`, and `raw`. The manual `docker`/`podman` commands below still work if you
prefer them.

> **Every command and flag** — scripts, `gnmi_peer`, `app` modes, `cli_app`,
> and the VPN binaries — is catalogued in
> [docs/commands.md](docs/commands.md).

### Manual build

```bash
docker build -t marvel:dev hackthon/
```

The Dockerfile compiles everything, runs all gtests (`ctest --output-on-failure`),
and produces a minimal runtime image.  To skip tests:

```bash
docker build --build-arg RUN_TESTS=OFF -t marvel:dev hackthon/
```

On hosts where **Podman** is installed instead of Docker, use `podman build`
(same flags):

```bash
podman build -t marvel:dev hackthon/
```

Images built inside a dev container via `docker build` are stored in Podman's
local image store on the host and can be listed with `podman images`.  See
[docs/services.md](docs/services.md#running-with-podman) for `podman run` and
`podman-compose` equivalents of every startup recipe.

### Dependencies (all resolved inside the container)

| Package              | Used by |
|----------------------|---------|
| `libnghttp2-dev`     | `http2_session`, `grpc_session` |
| `libprotobuf-dev`    | generated gNMI proto classes |
| `protobuf-compiler`  | `protoc --cpp_out` at build time |
| `libevent-dev`       | `evt_io`, `evt_base` |
| `libssl-dev`         | OpenSSL (linked transitively by libevent) |
| `liblua5.4-dev`      | `lua_engine` |
| `libgtest-dev`       | unit tests |

---

## CLI — gNMI Operations

The `cli_app` binary provides a readline REPL that can send gNMI operations
to a peer device.  Four Lua command files are shipped in `app/command/`:

| File | Command | gNMI operation |
|------|---------|----------------|
| `gnmi_get.lua`     | `gnmi_get`     | `GetRequest` |
| `gnmi_update.lua`  | `gnmi_update`  | `SetRequest.update[]` |
| `gnmi_replace.lua` | `gnmi_replace` | `SetRequest.replace[]` |
| `gnmi_delete.lua`  | `gnmi_delete`  | `SetRequest.delete[]` |

### Arguments (all commands)

| Key | Default | Description |
|-----|---------|-------------|
| `target` | `127.0.0.1` | IP address or hostname of the peer gNMI device |
| `port` | `9339` | TCP port |
| `prefix` | `/` | Common YANG path prefix shared by all paths in the request |
| `path` | — | Specific leaf or subtree relative to prefix |
| `value` | — | New value (SET/UPDATE/REPLACE only); plain string or JSON object |
| `encoding` | `JSON` | Wire encoding: `JSON`, `JSON_IETF`, `PROTO` |

### Path format

Both `prefix` and `path` use the YANG instance-identifier syntax:
```
/module:container/list[key=value]/leaf
```
Key predicates like `[name=eth0]` are parsed and encoded into
`gnmi.PathElem.key` maps.

### Examples

```
# Fetch the operational status of interface eth0
Marvel> gnmi_get target=192.168.1.1 prefix=/interfaces/interface[name=eth0] path=state/oper-status

# Update the description of interface eth0
Marvel> gnmi_update target=192.168.1.1 prefix=/interfaces/interface[name=eth0] path=config/description value=uplink-to-spine

# Replace the entire config subtree of eth0
Marvel> gnmi_replace target=192.168.1.1 prefix=/interfaces path=/interface[name=eth0]/config value={"description":"new-uplink","enabled":true}

# Delete interface eth0 from the configuration
Marvel> gnmi_delete target=192.168.1.1 prefix=/interfaces path=/interface[name=eth0]
```

### CLI → gNMI server via MQTT + VPN tunnel

When `MQTT_HOST` is set, the CLI routes through an MQTT broker and an OpenVPN
tunnel instead of connecting directly.  See
[docs/gnmi-mqtt-flow.md](docs/gnmi-mqtt-flow.md) for the full end-to-end flow,
payload format, topic scheme, and the current **response gap** (SetResponse is
not yet returned to the CLI).

### How it works (CLI → peer device, direct gRPC)

```
readline REPL
  └─ process_command()                  detect cmd_name == gnmi_*
       └─ handle_gnmi_get/update/...()
            ├─ parse_yang_path()        "/prefix" + "path" → gnmi::Path
            ├─ build gnmi proto         gnmi::GetRequest / SetRequest
            ├─ SerializeToString()      raw protobuf bytes
            └─ gnmi_client::call()      [app/src/gnmi_client.cpp]
                 ├─ connect_tcp()       blocking POSIX connect() to target:port
                 ├─ http2_session       client-side nghttp2 session
                 │    └─ send connection preface + SETTINGS
                 ├─ grpc_session::encode_frame()   5-byte length prefix
                 ├─ http2_session::submit_request  POST /gnmi.gNMI/<Method>
                 └─ blocking recv loop
                      └─ grpc_session::decode_frame()  strip 5-byte prefix
                           └─ print gnmi proto in text format
```

---

## gnmi_peer — two-pane peer-to-peer gNMI shell

The `gnmi_peer` binary is a config-driven, Claude-style ncurses terminal:

```
 Marvel gNMI · local :58989 → 127.0.0.1:58990        <- dim header

 ❯ gnmi set /a/b:5,/c/d:up                            <- scrolling transcript
 [set] OK, 2 result(s)
 [remote] UPDATE /x/y = 7                             (remote pushes, in cyan)
 ...

 ╭────────────────────────────────────────────╮      <- bordered input box
 │ ❯ gnmi get /a/b                             │
 ╰────────────────────────────────────────────╯
   set · get · help · quit                            <- dim hint
```

* The **input box** (bottom) issues `gnmi set` / `gnmi get` to the remote
  endpoint over direct gRPC-over-HTTP/2 (`gnmi_client::push_async`).
* The **transcript** shows results plus every operation the remote peer pushes
  into *our* local gNMI server (via `update_sink` from the Set handler),
  colour-coded on your terminal's own background (configurable — see below).

It is peer-to-peer: run one `gnmi_peer` on each side. Each runs a local gNMI
server (so the other side can push updates to it) and sends set/get to the
other's server.

### Configuration (`--config`, default `/app/command/endpoint.lua`)

```lua
return {
  ["local"]  = { endpoint = { ip = "0.0.0.0",   port = 58989 } },
  ["remote"] = { endpoint = { ip = "127.0.0.1", port = 58990 } },
  -- FQDN form is also accepted for either endpoint:
  --   ["remote"] = { endpoint = "peer.example.com:58990" },
  tls = { enabled = false, cert = "", key = "", ca = "" },
}
```

* `local.endpoint`  — where this process runs its own gNMI server.
* `remote.endpoint` — where `gnmi set/get` are sent.
* Each `endpoint` is either `{ ip = <string>, port = <number> }` **or** a single
  `"host:port"` string. `local`/`remote` are Lua keywords, so they must be
  written as the quoted keys `["local"]` / `["remote"]`.

### Commands

| Command | Effect |
|---------|--------|
| `gnmi set <xpath>:<value>[,<xpath>:<value>...]` | One `SetRequest` with one `update[]` per pair (role `ADMIN`) |
| `gnmi get <xpath>[,<xpath>...]`                 | One `GetRequest` for the listed paths (role `VIEWER`); updates shown as JSON |
| `gnmi subscribe <xpath>[,<xpath>...]`           | Open a `Subscribe` STREAM; streamed `SubscribeResponse` notifications render as JSON (`[sub] {…}`) |
| `help`                                          | Command help |
| `quit` / `exit` (or Ctrl-D)                     | Leave |

Incoming data (remote `[remote]` Set pushes, Get updates, and `[sub]`
subscribe notifications) is rendered as **JSON**. Subscribe is server-streaming
and **on-change**: the target sends `{"syncResponse":true}` (no initial dump —
this stub target has no datastore), then streams a `SubscribeResponse`
notification whenever a **real `Set`** touches a subscribed path (from any
connection). So: subscribe on one peer, `gnmi set` on another (or the same),
and the change streams to the subscriber. The stream stays open while idle.

`xpath` uses `/`-separated YANG form, e.g.
`/interfaces/interface[name=eth0]/config/mtu`. In each `xpath:value` pair the
value is everything after the **first** `:` (so module-qualified segments like
`module:container` are not supported in this shorthand).

### Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--config=<path>`   | `/app/command/endpoint.lua` | Lua endpoint config |
| `--headless=<bool>` | auto | `true`/`false` force the front-end; auto = ncurses on a TTY, line-mode otherwise |
| `--log=<path>`      | `/tmp/gnmi_peer.log` | Where component logs go in TUI mode (keeps the display clean) |

The **headless line-mode** (no ncurses) is auto-selected when stdin/stdout is
not a TTY — it reads one command per line from stdin and prints results and
`[remote] …` pushes to stdout, so it is pipe- and CI-friendly. Interactive use
picks the ncurses TUI.

### Build

`gnmi_peer` is built as part of the normal image build (it links ncurses, added
to the build/runtime stages of the Dockerfile) and is copied to
`/app/gnmi_peer`:

```bash
podman build -t marvel:dev hackthon/        # or: docker build …
# skip the gtest suite:
podman build --build-arg RUN_TESTS=OFF -t marvel:dev hackthon/
```

Native build (needs `libevent`, `libnghttp2`, `libprotobuf`+`protoc`, `liblua5.4`,
`libncurses`, `libssl` dev packages):

```bash
cmake -S hackthon -B build -DBUILD_TESTING=ON
cmake --build build -j"$(nproc)" --target gnmi_peer
./build/app/peer/gnmi_peer --config=hackthon/app/command/endpoint.lua
```

### Run — interactive (ncurses TUI)

Attach a TTY (`-it`) so the two-pane UI renders:

```bash
podman run --rm -it -p 58989:58989 \
  -v "$PWD/hackthon/app/command/endpoint.lua:/app/command/endpoint.lua:ro" \
  marvel:dev \
  /app/gnmi_peer --config=/app/command/endpoint.lua
```

Type `gnmi set /a/b:5,/c/d:up`, `gnmi get /a/b`, `help`, or `quit` in the top
pane; remote pushes appear in the bottom pane. Lines are colour-coded on your
terminal's own background (defaults: cyan = remote pushes, green = OK,
amber = errors, dim = echoed commands) — override any of these with a `colors`
table in the config (see `app/command/endpoint.lua`). Exit with
`quit`/`exit`/Ctrl-D (these restore the terminal via `endwin()`).

### Run — two peers (peer-to-peer)

Each side runs its own local server and points `remote` at the other. On one
host, put both on a user network so they resolve each other by name:

```bash
podman network create peer-net

# Peer B (receiver): local 58990, remote peerA:58989
podman run -d --name peerB --network peer-net \
  -v "$PWD/hackthon/app/command/endpoint.lua:/app/command/endpoint.lua:ro" \
  marvel:dev \
  /app/gnmi_peer --headless=true \
    --config=/app/command/endpoint.lua   # edit this config so local=58990, remote=peerA:58989

# Peer A (sender/interactive): local 58989, remote peerB:58990
podman run --rm -it --name peerA --network peer-net \
  -v "$PWD/hackthon/app/command/endpoint.lua:/app/command/endpoint.lua:ro" \
  marvel:dev \
  /app/gnmi_peer --config=/app/command/endpoint.lua
```

Give each peer its own config (`local`/`remote` swapped). A `gnmi set` in A's
top pane appears as `[remote] UPDATE …` in B's output.

### Run — two peers (docker-compose)

A ready-made two-peer stack lives at
[docs/docker-compose.gnmi-peer.yml](docs/docker-compose.gnmi-peer.yml) (configs
in `docs/gnmi-peer/`). Both peers run the TUI; attach to each in its own
terminal:

```bash
docker compose -f docs/docker-compose.gnmi-peer.yml up -d
docker compose -f docs/docker-compose.gnmi-peer.yml attach peerA   # terminal 1
docker compose -f docs/docker-compose.gnmi-peer.yml attach peerB   # terminal 2
# in peerA:  gnmi set /demo/leaf:5   → shows in peerB's bottom pane
# detach without stopping: Ctrl-P Ctrl-Q ;  tear down: … down
```

Use `podman-compose` (or `podman compose`) in place of `docker compose` on
Podman hosts. To watch pushes in the logs instead of attaching, add
`--headless=true` to a peer's command (see the file's header comment).

### Automated smoke test

`app/peer/test/smoke_two_peer.sh` bakes two configs into a throwaway image, runs
B (receiver) + A (sender) on a network, and asserts B received the pushed
updates:

```bash
podman build --build-arg RUN_TESTS=OFF -t marvel:dev hackthon/
IMAGE=marvel:dev hackthon/app/peer/test/smoke_two_peer.sh
#   → SMOKE TEST: PASS
```

---

## Adding a New gRPC Handler

Register a unary handler in `connected_client::register_gnmi_handlers()`
(`app/src/client_app.cpp`):

```cpp
m_grpc->register_unary("/mypackage.MyService/MyMethod",
    [](const std::string &req_pb) -> std::pair<int, std::string> {
        mypackage::MyRequest req;
        if (!req.ParseFromString(req_pb))
            return {3, ""};  // INVALID_ARGUMENT

        mypackage::MyResponse resp;
        // ... fill resp ...
        std::string out;
        resp.SerializeToString(&out);
        return {0, out};  // OK
    });
```

gRPC status codes: `0` OK, `3` INVALID_ARGUMENT, `5` NOT_FOUND, `12` UNIMPLEMENTED, `13` INTERNAL.
