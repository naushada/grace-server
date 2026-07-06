#!/usr/bin/env bash
#
# tunnel-smoke.sh — smoke-test `app --mode=grpc-tunnel-server` (openconfig
# grpctunnel) with grpcurl, no device needed.
#
# Simulates a tunnel client: opens grpctunnel.Tunnel/Register and sends
# Target{op:ADD, target_type:GNMI_GNOI}, then asserts the server acked it
# (RegisterOp with accept:true). This covers Increment A (target registration).
# The Tunnel data plane (byte proxy) is Increment B and needs a real client.
#
# Prereqs:
#   * grpcurl on PATH            (https://github.com/fullstorydev/grpcurl)
#   * this repo's app/idl protos (resolved relative to this script)
#   * a running server, plaintext h2c, at $HOST:$PORT, e.g.:
#       docker run -d --name tunnel -p 58989:58989 \
#         gnmiserver:dev /app/app --mode=grpc-tunnel-server --port=58989 --headless=true
#
# Usage:
#   HOST=127.0.0.1 PORT=58989 ./tunnel-smoke.sh
#   CONTAINER=tunnel ./tunnel-smoke.sh          # also tail the server's [reg] logs
set -uo pipefail

HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-58989}"
TARGET_ID="${TARGET_ID:-smoke-dev}"
TARGET_TYPE="${TARGET_TYPE:-GNMI_GNOI}"
CONTAINER="${CONTAINER:-}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDL="${IDL:-$here/../app/idl}"

die() { echo "tunnel-smoke: $*" >&2; exit 2; }
command -v grpcurl >/dev/null 2>&1 \
  || die "grpcurl not found — https://github.com/fullstorydev/grpcurl"
[ -f "$IDL/tunnel/tunnel.proto" ] || die "tunnel.proto not found under $IDL"

tmp="$(mktemp -d)"
out="$tmp/register.out"
trap 'rm -rf "$tmp"' EXIT

echo "[smoke] Register target '$TARGET_ID' ($TARGET_TYPE) …"
# Send one RegisterOp{target ADD}, then hold the stream ~4s to read the ack.
{ printf '{"target":{"op":"ADD","target":"%s","targetType":"%s"}}\n' \
    "$TARGET_ID" "$TARGET_TYPE"; sleep 4; } \
  | grpcurl -plaintext -import-path "$IDL" -proto tunnel/tunnel.proto \
      -d @ "$HOST:$PORT" grpctunnel.Tunnel/Register >"$out" 2>&1 || true

echo "[smoke] server replied:"
sed 's/^/    /' "$out" 2>/dev/null || true

if grep -qF '"accept": true' "$out" && grep -qF "$TARGET_ID" "$out"; then
  echo "[smoke] PASS — server acked Register for '$TARGET_ID'"
  rc=0
else
  echo "[smoke] FAIL — no accepted Register ack from the server"
  echo "[smoke]   check: server in --mode=grpc-tunnel-server? plaintext? right port?"
  rc=1
fi

if [ -n "$CONTAINER" ]; then
  echo "[smoke] server [reg] logs:"
  docker logs --tail 20 "$CONTAINER" 2>&1 | grep -a '\[reg\]' | sed 's/^/    /' || true
fi
exit "$rc"
