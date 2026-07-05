# Command Reference

Every command surface in this repo, in one place:

- [Helper scripts](#helper-scripts) — `build.sh`, `run.sh`
- [`gnmi_peer`](#gnmi_peer) — flags + interactive shell commands
- [`app`](#app-multi-mode-binary) — the multi-mode server/client/gnmi binary
- [`cli_app`](#cli_app-mqtt-repl) — the MQTT readline REPL
- [VPN / OpenVPN binaries](#vpn--openvpn-binaries)
- [gRPC status codes](#grpc-status-codes)

Binaries inside the image live under `/app/` (`/app/app`, `/app/cli_app`,
`/app/gnmi_peer`, `/app/vpn_server`, `/app/vpn_client`, `/app/openvpn_server`,
`/app/openvpn_client`). Run them directly, or via `run.sh` (below).

---

## Helper scripts

Both scripts live in `hackthon/` and pick **docker or podman** automatically
(docker preferred when its daemon is reachable, else podman). Override with
`--engine <docker|podman>` or `ENGINE=`.

### `build.sh`

Builds the image from the Dockerfile (context = `hackthon/`).

```
./build.sh [options]
```

| Option | Default | Description |
|--------|---------|-------------|
| `-e, --engine <docker\|podman>` | auto | Force the container engine |
| `-t, --image <name[:tag]>` | `marvel:release` | Image tag to build |
| `--tests <on\|off>` | `off` | Run the gtest suite (`ctest`) during the build |
| `--build-type <type>` | `Debug` | `CMAKE_BUILD_TYPE` |
| `--no-cache` | — | Build without layer cache |
| `--pull` | — | Attempt to pull a newer base image |
| `-- <args…>` | — | Pass any following args straight to `<engine> build` |
| `-h, --help` | — | Show help |

```bash
./build.sh                    # fast build, no tests
./build.sh --tests on         # build + run gtests
./build.sh -t marvel:dev --no-cache --build-type Release
```

### `run.sh`

Runs any binary in a container, or opens a shell in one.

```
./run.sh [global options] <command> [command args] [-- binary args]
```

**Commands**

| Command | Runs | Notes |
|---------|------|-------|
| `gnmi-peer` | `/app/gnmi_peer` | Two-pane TUI (interactive). `--headless` = line-mode. Default port 58989. |
| `gnmi-server` | `/app/app --mode=gnmi-server` | Plain gNMI server. Default port 58989. |
| `cli` | `/app/cli_app` | Interactive readline REPL. |
| `app` | `/app/app` | Pass the mode yourself, e.g. `-- --mode=client`. |
| `vpn-server` | `/app/vpn_server` | Auto: `--root` + TUN, port 1194. |
| `vpn-client` | `/app/vpn_client` | Auto: `--root` + TUN. |
| `openvpn-server` | `/app/openvpn_server` | Auto: `--root` + TUN, port 1194. |
| `openvpn-client` | `/app/openvpn_client` | Auto: `--root` + TUN. |
| `shell` | `/bin/bash` | Interactive bash in a fresh container. |
| `exec <name>` | `<engine> exec` | bash (or `-- <cmd>`) inside a running container. |
| `raw -- <cmd…>` | `<cmd…>` | Arbitrary command in a fresh container. |
| `smoke` | — | Healthcheck-gated two-peer smoke test: start peerB, gate on its health, send a `gnmi set` from peerA, assert peerB received it. Exits 0 (PASS) / 1 (FAIL). |

**Global options**

| Option | Default | Description |
|--------|---------|-------------|
| `-e, --engine <docker\|podman>` | auto | Force the engine |
| `--image <name[:tag]>` | `marvel:release` | Image to run |
| `--name <name>` | — | Container name |
| `--network <net>` | — | Attach to a network |
| `-p, --port <spec>` | per-command | Publish a port (repeatable), e.g. `58989:58989` |
| `--config <path>` | — | Host `endpoint.lua` to mount (gnmi-peer) |
| `--headless` | — | gnmi-peer line-mode (no TTY; adds `--headless=true`) |
| `-E, --env <K=V>` | — | Set an env var (repeatable) |
| `-d, --detach` | — | Run detached (background) |
| `--root` | — | Run as root user |
| `--tun` | — | Add `NET_ADMIN`/`NET_RAW` + `/dev/net/tun` |
| `--no-rm` | — | Keep the container after it exits |
| `--no-tty` | — | Do not allocate a TTY |
| `--build` | — | Run `./build.sh` first, then run |
| `-h, --help` | — | Show help |

TTY selection: `-d` → detached; `--headless`/`--no-tty` → `-i`; interactive
commands (`gnmi-peer`, `cli`, `shell`) → `-it`; server commands → attached, no TTY.

```bash
./run.sh gnmi-peer                          # interactive TUI, port 58989
./run.sh --config ./my.lua gnmi-peer        # mount a custom config
echo 'gnmi get /a/b' | ./run.sh --headless gnmi-peer
./run.sh --build gnmi-server -- --gnmi-port=9339   # build, then run
./run.sh -d --name peerB --network peer-net --headless gnmi-peer
./run.sh shell                              # poke around the image
./run.sh exec peerB                         # bash into a running container
./run.sh raw -- ls -la /app
```

---

## `gnmi_peer`

Config-driven, two-pane, peer-to-peer gNMI shell. See
[README → gnmi_peer](../README.md#gnmi_peer--two-pane-peer-to-peer-gnmi-shell)
for the full walkthrough.

### Flags

| Flag | Default | Description |
|------|---------|-------------|
| `--config <path>` | `/app/command/endpoint.lua` | Lua endpoint config (`local`/`remote`) |
| `--headless <bool>` | auto | `true`/`false` force the front-end; auto = ncurses on a TTY, line-mode otherwise |
| `--log <path>` | `/tmp/gnmi_peer.log` | Where component logs go in TUI mode |

### Interactive shell commands (top pane / stdin)

| Command | gNMI op | Notes |
|---------|---------|-------|
| `gnmi set <xpath>:<value>[,<xpath>:<value>…]` | `SetRequest.update[]` | Role `ADMIN`. One update per comma-pair; value is everything after the **first** `:`. |
| `gnmi get <xpath>[,<xpath>…]` | `GetRequest` | Role `VIEWER`. Updates shown as JSON. |
| `gnmi subscribe <xpath>[,<xpath>…]` | `Subscribe` (STREAM) | On-change: sends `{"syncResponse":true}`, then streams a `SubscribeResponse` (JSON, `[sub] {…}`) whenever a real `Set` touches a subscribed subtree — from any connection. `sub` is an alias. |
| `help` | — | Show in-shell help. |
| `quit` / `exit` (or Ctrl-D) | — | Leave (restores the terminal). |

Incoming data — remote Set pushes (`[remote] …`), Get updates, and Subscribe
notifications (`[sub] …`) — is rendered as **JSON**, e.g.
`[sub] {"update":{"timestamp":…,"update":[{"path":"/demo/leaf","val":3}]}}`.

`xpath` is `/`-separated YANG, e.g.
`/interfaces/interface[name=eth0]/config/mtu`. The `gnmi set` / `gnmi get`
prefix is optional (`set …` and `get …` also work). Operations the **remote**
peer pushes into this process's local server appear in the bottom pane as
`[remote] UPDATE/REPLACE/DELETE …`.

---

## `app` (multi-mode binary)

`/app/app --mode=<server|client|gnmi-server|gnmi-mqtt-client>` (default `server`).

### `--mode=gnmi-server` (standalone gNMI target — no VPN/TUN)

| Flag | Default | Description |
|------|---------|-------------|
| `--gnmi-port <port>` | `58989` | Plain TCP listen port |
| `--gnmi-tls-port <port>` | `0` | If set, run plain **and** TLS listeners simultaneously |
| `--gnmi-tls <bool>` | `false` | Single-port TLS mode (ignored when `--gnmi-tls-port` is set) |
| `--gnmi-cert/--gnmi-key/--gnmi-ca <path>` | — | PEM files for TLS |

### `--mode=server` (custom VPN tunnel server + gNMI)

| Flag | Default | Description |
|------|---------|-------------|
| `--server-ip <ip>` | `10.8.0.1` | Server TUN IP |
| `--pool-start <ip>` | `10.8.0.2` | First client IP |
| `--pool-end <ip>` | `10.8.0.254` | Last client IP |
| `--netmask <mask>` | `255.255.255.0` | Tunnel netmask |
| `--tls <bool>` + `--cert/--key/--ca` | `false` | VPN-tunnel TLS |
| `--gnmi-tls <bool>` + `--gnmi-cert/--gnmi-key/--gnmi-ca` | `false` | gNMI-server TLS |
| `--gnmi-push <bool>` | `false` | Push a gNMI Get to each client after the tunnel is up |
| `--gnmi-port <port>` | `58989` | Client gNMI port to push to |
| `--gnmi-push-delay <s>` | `2` | Seconds to wait before pushing |
| `--mqtt-host <host>` / `--mqtt-port <port>` | — / `1883` | Per-peer MQTT (subscribe `fwd/<vip>`) |

### `--mode=client` (custom VPN tunnel client + gNMI)

| Flag | Default | Description |
|------|---------|-------------|
| `--server <host>` | `127.0.0.1` | VPN server address |
| `--port <port>` | `1194` | VPN server port |
| `--status <path>` | `/run/vpn_status.lua` | Lua status file |
| `--gnmi-port <port>` | `58989` | gNMI server listen port |
| `--tls <bool>` + `--cert/--key/--ca` | `false` | VPN-tunnel TLS |
| `--gnmi-tls <bool>` + `--gnmi-cert/--gnmi-key/--gnmi-ca` | `false` | gNMI-server TLS |
| `--gnmi-probe <bool>` | `false` | Fire a one-shot gNMI Get to the server VIP |
| `--server-vip <ip>` | `10.8.0.1` | Server-side VPN IP to probe |

### `--mode=gnmi-mqtt-client` (MQTT relay)

| Flag | Default | Description |
|------|---------|-------------|
| `--mqtt-host <host>` | `localhost` | MQTT broker address |
| `--mqtt-port <port>` | `1883` | MQTT broker port |

Subscribes to `cli/#` and `resp/#`; republishes `cli/<ip>` → `fwd/<ip>` and
`resp/<ip>` → `cli_resp/<ip>`.

---

## `cli_app` (MQTT REPL)

Interactive `Marvel>` prompt. Connects to the broker via the `MQTT_HOST` /
`MQTT_PORT` environment variables. Publishes gNMI protos to `cli/<vip>` and
reads responses from `cli_resp/<vip>`.

| Command | gNMI op |
|---------|---------|
| `clients` | List VIPs currently connected through `openvpn-server` (local) |
| `help` | Show help |
| `gnmi_get <args>` | `GetRequest` |
| `gnmi_update <args>` | `SetRequest.update[]` |
| `gnmi_replace <args>` | `SetRequest.replace[]` |
| `gnmi_delete <args>` | `SetRequest.delete[]` |

Argument keys (`key=value`, space-separated):

| Key | Default | Description |
|-----|---------|-------------|
| `target` | `127.0.0.1` | Peer gNMI device IP / VIP |
| `port` | `9339` | TCP port |
| `prefix` | `/` | Common YANG path prefix |
| `path` | — | Leaf/subtree relative to prefix |
| `value` | — | New value (set/update/replace); string or JSON |
| `encoding` | `JSON` | `JSON`, `JSON_IETF`, `PROTO` |
| `role` | `VIEWER` (get) / `ADMIN` (set) | RBAC role in `prefix.target` |
| `tunnel_host` / `tunnel_port` | — / `1194` | Route via VPN tunnel first (update/replace/delete) |

---

## VPN / OpenVPN binaries

Detailed startup recipes (capabilities, TUN device, compose services) are in
[services.md](services.md). Flag summaries:

### `vpn_server` (custom protocol)

| Flag | Default | | Flag | Default |
|------|---------|-|------|---------|
| `--server-ip` | `10.8.0.1` | | `--tls` | `false` |
| `--pool-start` | `10.8.0.2` | | `--cert`/`--key`/`--ca` | — |
| `--pool-end` | `10.8.0.254` | | `--mqtt-host` | `localhost` (when set) |
| `--netmask` | `255.255.255.0` | | `--gnmi-port` | `58989` |
| `--port` | `1194` | | | |

### `vpn_client` (custom protocol)

| Flag | Default | | Flag | Default |
|------|---------|-|------|---------|
| `--server` | `127.0.0.1` | | `--tls` | `false` |
| `--port` | `1194` | | `--cert`/`--key`/`--ca` | — |
| `--server-vip` | `10.8.0.1` | | `--gnmi-probe` | `false` |
| `--status` | `/run/vpn_status.lua` | | `--gnmi-fwd-ip` | — |
| `--gnmi-port` | `58989` | | | |

### `openvpn_server` (wraps system `openvpn`)

| Flag | Default | | Flag | Default |
|------|---------|-|------|---------|
| `--port` | `1194` | | `--tls` | `false` |
| `--mgmt-port` | `7505` | | `--cert`/`--key`/`--ca` | — |
| `--gnmi-port` | `58989` | | `--mqtt-host` | `localhost` (when set) |

### `openvpn_client` (wraps system `openvpn`)

| Flag | Default | | Flag | Default |
|------|---------|-|------|---------|
| `--server` | `127.0.0.1` | | `--tls` | `false` |
| `--port` | `1194` | | `--cert`/`--key`/`--ca` | — |
| `--fwd-host` | `127.0.0.1` | | `--status` | `/run/openvpn_status.lua` |
| `--fwd-ports` | `80,443,58989` | | | |

---

## gRPC status codes

Returned by the gNMI handlers (`app/src/client_app.cpp`):

| Code | Meaning |
|------|---------|
| `0` | OK |
| `3` | INVALID_ARGUMENT |
| `5` | NOT_FOUND |
| `7` | PERMISSION_DENIED (e.g. non-`ADMIN` role on `Set`) |
| `12` | UNIMPLEMENTED (e.g. `Subscribe`) |
| `13` | INTERNAL |
