#!/usr/bin/env bash
#
# run.sh — run any marvel binary in a container with docker or podman
#          (whichever is present), or open a shell in one.
#
# The image is built by ./build.sh (default marvel:release) and bundles:
#   gnmi-peer  gnmi-server  cli  app  vpn-server  vpn-client
#   openvpn-server  openvpn-client
#
# Usage:
#   ./run.sh [global options] <command> [command args] [-- binary args]
#
# Commands:
#   gnmi-peer        Two-pane peer-to-peer gNMI shell (interactive TUI).
#                    Use --headless for line-mode (pipe/CI). --config mounts a
#                    host endpoint.lua. Default port 58989.
#   gnmi-server      Plain gNMI server: /app/app --mode=gnmi-server (port 58989).
#   grpc-tunnel-server  Accept dial-out tunnel sessions from targets behind NAT
#                    (/app/app --mode=grpc-tunnel-server, port 58989).
#   cli              Interactive readline CLI: /app/cli_app.
#   app              /app/app — pass the mode yourself, e.g. `-- --mode=client`.
#   vpn-server       /app/vpn_server        (auto: --root + TUN, port 1194).
#   vpn-client       /app/vpn_client        (auto: --root + TUN).
#   openvpn-server   /app/openvpn_server    (auto: --root + TUN, port 1194).
#   openvpn-client   /app/openvpn_client    (auto: --root + TUN).
#   shell            Interactive bash shell in a fresh container.
#   exec <name>      bash (or `-- <cmd>`) inside an already-running container.
#   raw -- <cmd...>  Run an arbitrary command in a fresh container.
#   smoke            Healthcheck-gated two-peer smoke test: start peerB, wait
#                    until it reports healthy, send a `gnmi set` from peerA, and
#                    assert peerB received it. Exits 0 (PASS) / 1 (FAIL).
#
# Global options:
#   -e, --engine <docker|podman>   Force the engine        (default: auto)
#       --image  <name[:tag]>      Image to run            (default: marvel:release)
#       --name   <name>            Container name
#       --network <net>            Attach to a network
#   -p, --port   <spec>            Publish a port (repeatable), e.g. 58989:58989
#       --config <path>            Host endpoint.lua to mount (gnmi-peer)
#       --headless                 gnmi-peer line-mode (no TTY, adds --headless=true)
#   -E, --env    <K=V>             Set an env var (repeatable)
#   -d, --detach                   Run detached (background)
#       --root                     Run as root user
#       --tun                      Add NET_ADMIN/NET_RAW + /dev/net/tun
#       --no-rm                    Keep the container after it exits
#       --no-tty                   Do not allocate a TTY
#       --build                    Run ./build.sh first, then run
#   -h, --help                     Show this help
#
# Env:
#   ENGINE   Same as --engine.   IMAGE   Same as --image.
#
# Examples:
#   ./run.sh gnmi-peer                         # interactive TUI, port 58989
#   ./run.sh --config ./my.lua gnmi-peer       # with a custom config
#   ./run.sh --build gnmi-server -- --gnmi-port=9339
#   echo 'gnmi get /a/b' | ./run.sh --headless gnmi-peer
#   ./run.sh --name peerB -d --network peer-net gnmi-peer --headless
#   ./run.sh shell                             # poke around inside the image
#   ./run.sh exec peerB                        # bash into a running container
#   ./run.sh smoke                             # healthcheck-gated two-peer test
set -eo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="${IMAGE:-marvel:release}"
ENGINE="${ENGINE:-}"
NAME=""
NETWORK=""
CONFIG=""
DETACH=0
ROOT=0
TUN=0
HEADLESS=0
NOTTY=0
RM=1
DOBUILD=0
PORTS=()
ENVS=()
POSITIONAL=()
PASSTHRU=()

die() { echo "run.sh: $*" >&2; exit 1; }
usage() { awk 'NR==1{next} /^set -e/{exit} {sub(/^# ?/,""); print}' "$0"; }

detect_engine() {
  if [ -n "$ENGINE" ]; then echo "$ENGINE"; return; fi
  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then echo docker; return; fi
  if command -v podman >/dev/null 2>&1; then echo podman; return; fi
  if command -v docker >/dev/null 2>&1; then echo docker; return; fi
  echo ""
}

while [ $# -gt 0 ]; do
  case "$1" in
    -e|--engine)  ENGINE="$2"; shift 2 ;;
    --image)      IMAGE="$2"; shift 2 ;;
    --name)       NAME="$2"; shift 2 ;;
    --network)    NETWORK="$2"; shift 2 ;;
    -p|--port)    PORTS+=("$2"); shift 2 ;;
    --config)     CONFIG="$2"; shift 2 ;;
    --headless)   HEADLESS=1; shift ;;
    -E|--env)     ENVS+=("$2"); shift 2 ;;
    -d|--detach)  DETACH=1; shift ;;
    --root)       ROOT=1; shift ;;
    --tun)        TUN=1; shift ;;
    --no-rm)      RM=0; shift ;;
    --no-tty)     NOTTY=1; shift ;;
    --build)      DOBUILD=1; shift ;;
    -h|--help)    usage; exit 0 ;;
    --)           shift; PASSTHRU=("$@"); break ;;
    -*)           die "unknown option: $1 (try --help)" ;;
    *)            POSITIONAL+=("$1"); shift ;;
  esac
done

cmd="${POSITIONAL[0]}"
[ -n "$cmd" ] || { usage; exit 1; }

engine="$(detect_engine)"
[ -n "$engine" ] || die "no container engine found — install docker or podman"

if [ "$DOBUILD" = 1 ]; then
  "$here/build.sh" --engine "$engine" --image "$IMAGE"
fi

# ---- per-command defaults -------------------------------------------------
INTERACTIVE=0
BIN=()
inner_cfg="/app/command/endpoint.lua"

add_default_port() { [ ${#PORTS[@]} -eq 0 ] && PORTS+=("$1"); }

case "$cmd" in
  gnmi-peer)
    INTERACTIVE=1
    add_default_port "58989:58989"
    BIN=(/app/gnmi_peer "--config=$inner_cfg")
    [ "$HEADLESS" = 1 ] && BIN+=(--headless=true)
    ;;
  gnmi-server)
    add_default_port "58989:58989"
    BIN=(/app/app --mode=gnmi-server)
    ;;
  grpc-tunnel-server)
    add_default_port "58989:58989"
    BIN=(/app/app --mode=grpc-tunnel-server)
    ;;
  cli)
    INTERACTIVE=1
    BIN=(/app/cli_app)
    ;;
  app)
    BIN=(/app/app)
    ;;
  vpn-server)
    ROOT=1; TUN=1; add_default_port "1194:1194"
    BIN=(/app/vpn_server)
    ;;
  vpn-client)
    ROOT=1; TUN=1
    BIN=(/app/vpn_client)
    ;;
  openvpn-server)
    ROOT=1; TUN=1; add_default_port "1194:1194"
    BIN=(/app/openvpn_server)
    ;;
  openvpn-client)
    ROOT=1; TUN=1
    BIN=(/app/openvpn_client)
    ;;
  shell)
    INTERACTIVE=1
    BIN=(/bin/bash)
    ;;
  exec)
    # exec into a running container: run.sh exec <name> [-- cmd...]
    target="${POSITIONAL[1]:-$NAME}"
    [ -n "$target" ] || die "exec needs a container name: run.sh exec <name>"
    ecmd=(/bin/bash)
    [ ${#PASSTHRU[@]} -gt 0 ] && ecmd=("${PASSTHRU[@]}")
    etty="-it"; [ "$NOTTY" = 1 ] && etty="-i"
    set -x
    exec "$engine" exec $etty "$target" "${ecmd[@]}"
    ;;
  smoke)
    # Healthcheck-gated two-peer smoke test. Builds a throwaway image with two
    # baked configs (no host mounts), starts peerB with a container healthcheck,
    # GATES on the engine's reported health status (not sleeps/log-greps), then
    # sends a `gnmi set` from a one-off headless peerA and asserts peerB received
    # it. Engine-uniform (docker or podman). Exits 0 on PASS, 1 on FAIL.
    smoke_img="gnmi-peer-smoke:local"
    net="gnmi-peer-smoke-net"
    cb="gnmi-peer-smoke-b"; ca="gnmi-peer-smoke-a"
    "$engine" image inspect "$IMAGE" >/dev/null 2>&1 \
      || die "image '$IMAGE' not found — run ./build.sh first"
    tmp="$(mktemp -d)"
    smoke_cleanup() {
      "$engine" rm -f "$ca" "$cb" >/dev/null 2>&1 || true
      "$engine" network rm "$net" >/dev/null 2>&1 || true
      "$engine" rmi -f "$smoke_img" >/dev/null 2>&1 || true
      rm -rf "$tmp"
    }
    trap smoke_cleanup EXIT
    cat > "$tmp/peerA.lua" <<'LUA'
return { ["local"] = { endpoint = { ip = "0.0.0.0", port = 58989 } },
         ["remote"] = { endpoint = "peerB:58990" } }
LUA
    cat > "$tmp/peerB.lua" <<'LUA'
return { ["local"] = { endpoint = { ip = "0.0.0.0", port = 58990 } },
         ["remote"] = { endpoint = "peerA:58989" } }
LUA
    cat > "$tmp/Containerfile" <<EOF
FROM $IMAGE
COPY --chown=edge:cordoba peerA.lua /app/peerA.lua
COPY --chown=edge:cordoba peerB.lua /app/peerB.lua
EOF
    echo "[smoke] building test image with $engine …"
    "$engine" build -t "$smoke_img" "$tmp" >/dev/null
    "$engine" rm -f "$ca" "$cb" >/dev/null 2>&1 || true
    "$engine" network create "$net" >/dev/null 2>&1 || true
    echo "[smoke] starting peerB (receiver) with a healthcheck …"
    "$engine" run -d --name "$cb" --network "$net" --network-alias peerB \
      --health-cmd "ss -ltnH 'sport = :58990' | grep -q ." \
      --health-interval 3s --health-timeout 3s --health-retries 5 \
      --health-start-period 2s \
      "$smoke_img" /app/gnmi_peer --config=/app/peerB.lua --headless=true >/dev/null
    echo "[smoke] gating on peerB health …"
    status=""
    for _ in $(seq 1 25); do
      status="$("$engine" inspect "$cb" --format '{{.State.Health.Status}}' 2>/dev/null || echo)"
      if [ "$status" = healthy ] || [ "$status" = unhealthy ]; then break; fi
      read -t 2 </dev/null || true
    done
    if [ "$status" != healthy ]; then
      echo "[smoke] FAIL: peerB never became healthy (status='${status:-none}')"
      "$engine" logs "$cb" 2>&1 | tail -5
      exit 1
    fi
    echo "[smoke] peerB healthy — sending 'gnmi set' from peerA …"
    # Pipe the command INSIDE the container so EOF is a real in-container pipe;
    # gnmi_peer sends the set then self-exits (relying on `run -i` stdin EOF
    # forwarding across the podman/docker boundary is unreliable).
    "$engine" run -d --name "$ca" --network "$net" "$smoke_img" \
      sh -c "echo 'gnmi set /smoke/leaf:5,/smoke/x:up' | /app/gnmi_peer --config=/app/peerA.lua --headless=true" >/dev/null
    # Wait (bounded) for peerB to log the pushed updates.
    for _ in $(seq 1 15); do
      if "$engine" logs "$cb" 2>&1 | grep -qF '"path":"/smoke/x","val":"up"'; then break; fi
      read -t 1 </dev/null || true
    done
    logs="$("$engine" logs "$cb" 2>&1 || true)"
    aout="$("$engine" logs "$ca" 2>&1 || true)"
    echo "$aout" | grep -E '^\[set\]' || true
    fail=0
    echo "$aout" | grep -qE '\[set\] OK, 2 result' || { echo "[smoke] FAIL: peerA did not get 2 results"; fail=1; }
    echo "$logs" | grep -qF '"op":"UPDATE","path":"/smoke/leaf","val":"5"' || { echo "[smoke] FAIL: peerB missing /smoke/leaf"; fail=1; }
    echo "$logs" | grep -qF '"op":"UPDATE","path":"/smoke/x","val":"up"'    || { echo "[smoke] FAIL: peerB missing /smoke/x"; fail=1; }
    [ "$fail" = 0 ] && echo "[smoke] PASS" || echo "[smoke] FAIL"
    exit "$fail"
    ;;
  raw)
    [ ${#PASSTHRU[@]} -gt 0 ] || die "raw needs a command after --"
    ;;
  *)
    die "unknown command: $cmd (try --help)"
    ;;
esac

# ---- assemble `run` invocation --------------------------------------------
run_args=()
[ "$RM" = 1 ] && run_args+=(--rm)
[ -n "$NAME" ] && run_args+=(--name "$NAME")
[ -n "$NETWORK" ] && run_args+=(--network "$NETWORK")
for p in "${PORTS[@]}"; do run_args+=(-p "$p"); done
for ev in "${ENVS[@]}"; do run_args+=(-e "$ev"); done
[ "$ROOT" = 1 ] && run_args+=(--user root)
if [ "$TUN" = 1 ]; then
  run_args+=(--cap-add NET_ADMIN --cap-add NET_RAW --device /dev/net/tun)
fi

# Mount a host config over the baked-in endpoint.lua (gnmi-peer).
if [ -n "$CONFIG" ]; then
  [ -f "$CONFIG" ] || die "config not found: $CONFIG"
  cfg_abs="$(cd "$(dirname "$CONFIG")" && pwd)/$(basename "$CONFIG")"
  run_args+=(-v "$cfg_abs:$inner_cfg:ro")
fi

# TTY selection.
tty_flags=()
if [ "$DETACH" = 1 ]; then
  tty_flags=(-d)
elif [ "$HEADLESS" = 1 ] || [ "$NOTTY" = 1 ]; then
  tty_flags=(-i)
elif [ "$INTERACTIVE" = 1 ]; then
  tty_flags=(-it)
fi

# Final command line inside the container.
final=()
if [ "$cmd" = "raw" ]; then
  final=("${PASSTHRU[@]}")
else
  final=("${BIN[@]}")
  [ ${#PASSTHRU[@]} -gt 0 ] && final+=("${PASSTHRU[@]}")
fi

set -x
exec "$engine" run "${tty_flags[@]}" "${run_args[@]}" "$IMAGE" "${final[@]}"
