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
#   ./service.sh up         Build if needed, then bring up both services (detached)
#   ./service.sh attach     Attach to the gnmi_peer shell (gnmi get/set/subscribe)
#   ./service.sh logs [svc] Tail logs (default: tunnel — shows target registrations)
#   ./service.sh ps         Show service status
#   ./service.sh down       Stop and remove the stack
#   ./service.sh restart    down + up
#   ./service.sh build      Build the marvel:dev image (./build.sh)
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
  attach|peer)
    echo "[service] attaching to gnmi_peer — detach with Ctrl-P Ctrl-Q, quit the shell with 'quit'"
    exec $COMPOSE -f "$COMPOSE_FILE" attach peer
    ;;
  -h|--help|help)
    awk 'NR==1{next} /^set -e/{exit} {sub(/^# ?/,""); print}' "$0"
    ;;
  *) die "unknown command '$cmd' (up|attach|logs|ps|down|restart|build; --help)" ;;
esac
