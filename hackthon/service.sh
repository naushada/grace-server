#!/usr/bin/env bash
#
# service.sh — bring up the gRPC tunnel + gnmi_peer stack (a docker/podman
# compose wrapper around docs/docker-compose.tunnel.yml).
#
# Launches BOTH services so an end user can do gNMI without knowing internals:
#   * grpc-tunnel-server — devices dial :58989; a target is mapped to :9339
#   * gnmi_peer          — remote = tunnel:9339; the gNMI shell you drive
#
# Before `up`, set your device's PUBLISHED target in docs/tunnel.lua
# (listeners["9339"] = "<published target>"). For mTLS, set docs/tunnel.lua's
# tls table and mount the certs (see the compose file).
#
# Usage:
#   ./service.sh up                 Build if needed, then bring up both services
#   ./service.sh attach             Attach to the gnmi_peer shell (interactive)
#   ./service.sh get <path>         One-shot gNMI Get over the tunnel (scriptable)
#   ./service.sh set <path>:<val>   One-shot gNMI Set
#   ./service.sh subscribe <path>   Stream telemetry over the tunnel (Ctrl-C to stop)
#   ./service.sh mgmt [--out-file <path>] [--req-dir <dir>]
#                                   tNMI mgmt dial-out server + command TUI
#                                   (device dials in; type commands or
#                                   `send /req/<file>.lua`). --out-file saves
#                                   responses; --req-dir mounts a request-.lua
#                                   dir at /req (default: docs/mgmt-requests).
#   ./service.sh logs [svc]         Tail logs (default: tunnel — target registrations)
#   ./service.sh ps                 Show service status
#   ./service.sh down               Stop and remove the stack
#   ./service.sh restart            down + up
#   ./service.sh build              Build the marvel:dev image (./build.sh)
#
# get/set/subscribe run an ephemeral gnmi_peer against the tunnel — the stack
# must be up first (./service.sh up) so the device is registered.
#
# Env: ENGINE=docker|podman (default: auto).
set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
COMPOSE_FILE="$here/docs/docker-compose.tunnel.yml"
IMAGE="${IMAGE:-marvel:dev}"

die() { echo "service.sh: $*" >&2; exit 1; }

# Resolve a compose command: `docker compose`, `podman compose`, or podman-compose.
detect_compose() {
  case "${ENGINE:-}" in
    docker) command -v docker >/dev/null 2>&1 && { echo "docker compose"; return; } ;;
    podman)
      if podman compose version >/dev/null 2>&1; then echo "podman compose"; return; fi
      command -v podman-compose >/dev/null 2>&1 && { echo "podman-compose"; return; } ;;
  esac
  if command -v docker >/dev/null 2>&1 && docker compose version >/dev/null 2>&1; then
    echo "docker compose"; return
  fi
  if command -v podman >/dev/null 2>&1 && podman compose version >/dev/null 2>&1; then
    echo "podman compose"; return
  fi
  command -v podman-compose >/dev/null 2>&1 && { echo "podman-compose"; return; }
  command -v docker >/dev/null 2>&1 && { echo "docker compose"; return; }
  echo ""
}

# The engine binary (for image checks), inferred from the compose command.
engine_bin() { case "$COMPOSE" in podman*) echo podman ;; *) echo docker ;; esac; }

COMPOSE="$(detect_compose)"
[ -n "$COMPOSE" ] || die "no compose tool found (docker compose / podman compose)"
[ -f "$COMPOSE_FILE" ] || die "compose file not found: $COMPOSE_FILE"

dc() { $COMPOSE -f "$COMPOSE_FILE" "$@"; }

cmd="${1:-up}"
[ $# -gt 0 ] && shift || true

case "$cmd" in
  build)
    "$here/build.sh" -t "$IMAGE"
    ;;
  up)
    # Build the image if it isn't present yet.
    if ! "$(engine_bin)" image inspect "$IMAGE" >/dev/null 2>&1; then
      echo "[service] image '$IMAGE' not found — building …"
      "$here/build.sh" -t "$IMAGE"
    fi
    dc up -d "$@"
    echo
    echo "[service] stack up. Confirm the device registered:"
    echo "    ./service.sh logs | grep '+target'"
    echo "[service] then drive gNMI (tunnel is invisible):"
    echo "    ./service.sh attach     # gnmi get /system/state | gnmi set /a/b:5 | gnmi subscribe /..."
    ;;
  down)      dc down "$@" ;;
  restart)   dc down; dc up -d ;;
  ps|status) dc ps "$@" ;;
  logs)      dc logs -f "${1:-tunnel}" ;;
  get|set|subscribe|sub)
    # One-shot headless gNMI over the tunnel (no attach; good for scripting).
    # Runs an ephemeral gnmi_peer that connects to tunnel:9339, sends the
    # command, prints the result, and exits. `subscribe` streams until Ctrl-C.
    [ $# -gt 0 ] || die "$cmd needs a gNMI path, e.g. ./service.sh get /system/state"
    verb="$cmd"; [ "$verb" = sub ] && verb="subscribe"
    # Pass the command via an env var to avoid shell-quoting the path.
    if [ "$verb" = subscribe ]; then
      dc run --rm -T -e "GNMI_CMD=gnmi $verb $*" peer sh -c \
        '{ printf "%s\n" "$GNMI_CMD"; sleep 315360000; } | /app/gnmi_peer --config=/app/command/endpoint.lua --headless=true'
    else
      dc run --rm -T -e "GNMI_CMD=gnmi $verb $*" peer sh -c \
        'printf "%s\n" "$GNMI_CMD" | /app/gnmi_peer --config=/app/command/endpoint.lua --headless=true'
    fi
    ;;
  attach|peer)
    echo "[service] attaching to gnmi_peer — detach with Ctrl-P Ctrl-Q, quit the shell with 'quit'"
    exec $COMPOSE -f "$COMPOSE_FILE" attach peer
    ;;
  mgmt)
    # tNMI mgmt dial-out: a single-container server + command TUI (not compose).
    # A device dials :58989 and opens DialTcc.Subscribe; type CLI commands in the
    # TUI to send, results/pushes stream back. --out-file <path> also saves every
    # received response to a host file.
    out=""; reqdir="$here/docs/mgmt-requests"
    while [ $# -gt 0 ]; do
      case "$1" in
        --out-file) out="$2"; shift 2 ;;
        --req-dir)  reqdir="$2"; shift 2 ;;
        *) die "mgmt: unknown option '$1' (--out-file <path> | --req-dir <path>)" ;;
      esac
    done
    eng="$(engine_bin)"
    if ! "$eng" image inspect "$IMAGE" >/dev/null 2>&1; then
      echo "[service] image '$IMAGE' not found — building …"
      "$here/build.sh" -t "$IMAGE"
    fi
    runargs=(-it --rm -p 58989:58989)
    binargs=(/app/app --mode=mgmt-dialout)
    # Mount the request .lua dir at /req so `send /req/<file>.lua` works.
    if [ -d "$reqdir" ]; then
      ra="$(cd "$reqdir" && pwd)"
      runargs+=(-v "$ra:/req:ro")
    fi
    if [ -n "$out" ]; then
      od="$(dirname "$out")"; mkdir -p "$od" || die "cannot create dir: $od"
      oa="$(cd "$od" && pwd)"
      runargs+=(-v "$oa:/out")
      binargs+=("--log-file=/out/$(basename "$out")")
    fi
    echo "[service] mgmt dial-out on :58989 — device dials in; type commands or"
    echo "          'send /req/<file>.lua' in the TUI (^D to quit)"
    exec "$eng" run "${runargs[@]}" "$IMAGE" "${binargs[@]}"
    ;;
  -h|--help|help)
    awk 'NR==1{next} /^set -e/{exit} {sub(/^# ?/,""); print}' "$0"
    ;;
  *) die "unknown command '$cmd' (up|attach|get|set|subscribe|mgmt|logs|ps|down|restart|build; --help)" ;;
esac
