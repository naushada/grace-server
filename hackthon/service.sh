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
# Two "start" verbs; they share :58989, so starting one stops the other:
#   ./service.sh grpc-tunnel        Bring up the tunnel stack (tunnel-svc + peer-svc)
#   ./service.sh mgmt [--out-file <path>] [--req-dir <dir>]
#                                   tNMI mgmt dial-out server + command TUI
#                                   (mgmt-svc). --out-file saves responses;
#                                   --req-dir mounts a request-.lua dir at /req.
#
# Lifecycle verbs act on whichever is running (tunnel or mgmt):
#   ./service.sh attach             Attach to the running console (peer or mgmt)
#   ./service.sh logs [svc]         Tail logs of the running service
#   ./service.sh stop               Stop + remove whatever is up
#   ./service.sh restart            Restart whatever is up (re-attaches mgmt)
#   ./service.sh ps                 Show status
#   ./service.sh build              Build the marvel:dev image (./build.sh)
#
# Tunnel-only one-shots (need `grpc-tunnel` up first):
#   ./service.sh get <path>         One-shot gNMI Get over the tunnel
#   ./service.sh set <path>:<val>   One-shot gNMI Set
#   ./service.sh subscribe <path>   Stream telemetry (Ctrl-C to stop)
#
# `up` is a deprecated alias for `grpc-tunnel`; `down` for `stop`.
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
eng="$(engine_bin)"
ensure_image() {
  "$eng" image inspect "$IMAGE" >/dev/null 2>&1 && return 0
  echo "[service] image '$IMAGE' not found — building …"
  "$here/build.sh" -t "$IMAGE"
}
mgmt_exists() { "$eng" container inspect mgmt-svc >/dev/null 2>&1; }

cmd="${1:-help}"
[ $# -gt 0 ] && shift || true

case "$cmd" in
  build)
    "$here/build.sh" -t "$IMAGE"
    ;;
  grpc-tunnel|tunnel|up)
    ensure_image
    "$eng" rm -f mgmt-svc >/dev/null 2>&1 || true   # free :58989 if mgmt was up
    dc up -d "$@"
    echo
    echo "[service] tunnel up (tunnel-svc + peer-svc). Next:"
    echo "  1. ./service.sh logs        # wait for '[reg] +target …' — the server prints the"
    echo "                              #   exact device target and the docs/tunnel.lua line to add"
    echo "  2. put that target in docs/tunnel.lua listeners[\"9339\"], then: ./service.sh restart"
    echo "  3. ./service.sh attach      # gnmi get /system/state | set /a/b:5 | subscribe /..."
    echo "     (or one-shot: ./service.sh get /system/state)"
    ;;
  stop|down)
    dc down 2>/dev/null || true
    "$eng" rm -f mgmt-svc >/dev/null 2>&1 || true
    ;;
  restart)
    if mgmt_exists; then
      "$eng" restart mgmt-svc >/dev/null
      echo "[service] mgmt-svc restarted — attaching (detach: Ctrl-P Ctrl-Q; quit: ^D)"
      exec "$eng" attach mgmt-svc
    else
      dc down; dc up -d
      echo "[service] tunnel restarted — ./service.sh logs to grab the target"
    fi
    ;;
  ps|status)
    dc ps
    mgmt_exists && "$eng" ps -a --filter name=mgmt-svc || true
    ;;
  logs)
    if mgmt_exists; then exec "$eng" logs -f mgmt-svc
    else dc logs -f "${1:-tunnel}"; fi
    ;;
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
    if mgmt_exists; then
      "$eng" start mgmt-svc >/dev/null 2>&1 || true
      echo "[service] attaching to mgmt-svc — detach: Ctrl-P Ctrl-Q, quit: ^D"
      exec "$eng" attach mgmt-svc
    else
      echo "[service] attaching to gnmi_peer — detach: Ctrl-P Ctrl-Q, quit the shell with 'quit'"
      exec $COMPOSE -f "$COMPOSE_FILE" attach peer
    fi
    ;;
  mgmt)
    # tNMI mgmt dial-out: a detached, named container (mgmt-svc) you attach to,
    # so stop/restart/logs work like the tunnel. Shares :58989, so the tunnel is
    # stopped first. --out-file saves responses; --req-dir mounts the request dir.
    out=""; reqdir="$here/docs/mgmt-requests"
    while [ $# -gt 0 ]; do
      case "$1" in
        --out-file) out="$2"; shift 2 ;;
        --req-dir)  reqdir="$2"; shift 2 ;;
        *) die "mgmt: unknown option '$1' (--out-file <path> | --req-dir <path>)" ;;
      esac
    done
    ensure_image
    dc down >/dev/null 2>&1 || true                 # free :58989 if the tunnel was up
    "$eng" rm -f mgmt-svc >/dev/null 2>&1 || true
    # -d -it: detached with a TTY so the ncurses TUI runs and we can attach.
    # TERM forced (tmux's tmux-256color may be absent from the container terminfo).
    runargs=(-d -it --name mgmt-svc -e "TERM=${MGMT_TERM:-xterm-256color}" -p 58989:58989)
    binargs=(/app/app --mode=mgmt-dialout)
    if [ -d "$reqdir" ]; then
      ra="$(cd "$reqdir" && pwd)"; runargs+=(-v "$ra:/req:ro")
    fi
    if [ -n "$out" ]; then
      od="$(dirname "$out")"; mkdir -p "$od" || die "cannot create dir: $od"
      oa="$(cd "$od" && pwd)"
      runargs+=(-v "$oa:/out"); binargs+=("--log-file=/out/$(basename "$out")")
    fi
    "$eng" run "${runargs[@]}" "$IMAGE" "${binargs[@]}" >/dev/null
    echo "[service] mgmt-svc up on :58989 — attaching (detach: Ctrl-P Ctrl-Q; quit: ^D)"
    exec "$eng" attach mgmt-svc
    ;;
  -h|--help|help)
    awk 'NR==1{next} /^set -e/{exit} {sub(/^# ?/,""); print}' "$0"
    ;;
  *) die "unknown command '$cmd' (grpc-tunnel|mgmt|attach|get|set|subscribe|logs|ps|stop|restart|build; --help)" ;;
esac
