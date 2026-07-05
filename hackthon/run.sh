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
#   cli              Interactive readline CLI: /app/cli_app.
#   app              /app/app — pass the mode yourself, e.g. `-- --mode=client`.
#   vpn-server       /app/vpn_server        (auto: --root + TUN, port 1194).
#   vpn-client       /app/vpn_client        (auto: --root + TUN).
#   openvpn-server   /app/openvpn_server    (auto: --root + TUN, port 1194).
#   openvpn-client   /app/openvpn_client    (auto: --root + TUN).
#   shell            Interactive bash shell in a fresh container.
#   exec <name>      bash (or `-- <cmd>`) inside an already-running container.
#   raw -- <cmd...>  Run an arbitrary command in a fresh container.
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
