#!/usr/bin/env bash
#
# tunnel-smoke.sh — smoke-test `app --mode=grpc-tunnel-server` with grpcurl.
#
# Simulates a dial-out target with grpcurl: opens /tunnel.Tunnel/Session, sends
# a Register frame, and holds the stream open. Then sends an operator gNMI Get
# (prefix.target = the registered id) and asserts the server forwarded it DOWN
# the tunnel — i.e. the target's Session stream received a TunnelRequest for
# /gnmi.gNMI/Get.
#
# It checks the forward direction only. A full round-trip (target replies, the
# operator Get completes) needs a REAL target: grpcurl can't react to a received
# TunnelRequest by sending a matching TunnelResponse. So the operator Get here
# intentionally does not complete.
#
# Prereqs:
#   * grpcurl on PATH            (https://github.com/fullstorydev/grpcurl)
#   * this repo's app/idl protos (resolved relative to this script)
#   * a running tunnel server, plaintext h2c, reachable at $HOST:$PORT, e.g.:
#       ./run.sh --image gnmiserver:dev grpc-tunnel-server
#     (or) docker run -d --name tunnel -p 58989:58989 \
#            gnmiserver:dev /app/app --mode=grpc-tunnel-server --port=58989
#
# Usage:
#   HOST=127.0.0.1 PORT=58989 ./tunnel-smoke.sh
#   CONTAINER=tunnel ./tunnel-smoke.sh          # also tail the server's [tunnel] logs
set -uo pipefail

HOST="${HOST:-127.0.0.1}"
PORT="${PORT:-58989}"
TARGET_ID="${TARGET_ID:-smoke-dev}"
CONTAINER="${CONTAINER:-}"   # optional docker container name for server-log output
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IDL="${IDL:-$here/../app/idl}"

die() { echo "tunnel-smoke: $*" >&2; exit 2; }
command -v grpcurl >/dev/null 2>&1 \
  || die "grpcurl not found — https://github.com/fullstorydev/grpcurl"
[ -f "$IDL/tunnel/tunnel.proto" ] || die "tunnel.proto not found under $IDL"
[ -f "$IDL/gnmi/gnmi.proto" ]     || die "gnmi.proto not found under $IDL"

tmp="$(mktemp -d)"
target_out="$tmp/target.out"
target_pid=""
cleanup() { [ -n "$target_pid" ] && kill "$target_pid" 2>/dev/null; rm -rf "$tmp"; }
trap cleanup EXIT

# 1. Simulate the target: register, then hold the stream open ~15s so the server
#    stays registered while we drive the Get. Received TunnelRequests land here.
echo "[smoke] target '$TARGET_ID': opening Session + Register …"
{ printf '{"register":{"targetId":"%s"}}\n' "$TARGET_ID"; sleep 15; } \
  | grpcurl -plaintext -import-path "$IDL" -proto tunnel/tunnel.proto \
      -d @ "$HOST:$PORT" tunnel.Tunnel/Session >"$target_out" 2>&1 &
target_pid=$!

sleep 2   # let Register land

# 2. Operator Get for that target — will not complete (no real reply); bound it.
echo "[smoke] operator gNMI Get (prefix.target=$TARGET_ID) …"
timeout 5 grpcurl -plaintext -import-path "$IDL" -proto gnmi/gnmi.proto \
  -d "{\"prefix\":{\"target\":\"$TARGET_ID\"},\"path\":[{\"elem\":[{\"name\":\"system\"}]}]}" \
  "$HOST:$PORT" gnmi.gNMI/Get >"$tmp/get.out" 2>&1 || true

sleep 1

echo "[smoke] target stream received:"
sed 's/^/    /' "$target_out" 2>/dev/null || true

# 3. Assert the forward reached the target's stream.
if grep -qF '"method"' "$target_out" && grep -qF 'gnmi.gNMI/Get' "$target_out"; then
  echo "[smoke] PASS — server forwarded the Get down the tunnel to '$TARGET_ID'"
  rc=0
else
  echo "[smoke] FAIL — no forwarded TunnelRequest on the target stream"
  echo "[smoke]   check: server in --mode=grpc-tunnel-server? Register accepted?"
  echo "[smoke]   grpcurl Get output:"; sed 's/^/    /' "$tmp/get.out" 2>/dev/null || true
  rc=1
fi

if [ -n "$CONTAINER" ]; then
  echo "[smoke] server [tunnel] logs:"
  docker logs --tail 30 "$CONTAINER" 2>&1 | grep -a '\[tunnel\]' | sed 's/^/    /' || true
fi
exit "$rc"
