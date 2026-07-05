#!/usr/bin/env bash
# Two-peer smoke test for gnmi_peer.
#
# Peer B runs a local gNMI server (receiver). Peer A sends a multi-pair
# `gnmi set` to B over direct gRPC. We assert B's server accepted the ADMIN Set
# and emitted each pushed operation through update_sink (the lines the TUI would
# render in its bottom pane).
#
# Uses the headless line-mode of gnmi_peer (auto-selected off a TTY) so the run
# is fully scriptable. Requires podman (or set ENGINE=docker) and an image built
# from hackthon/ that contains /app/gnmi_peer.
#
# Usage:
#   podman build --build-arg RUN_TESTS=OFF -t marvel:release hackthon/
#   IMAGE=marvel:release hackthon/app/peer/test/smoke_two_peer.sh
set -euo pipefail

ENGINE="${ENGINE:-podman}"
IMAGE="${IMAGE:-marvel:release}"
NET="${NET:-gnmi-peer-smoke-net}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; $ENGINE rm -f peerA peerB >/dev/null 2>&1 || true; \
      $ENGINE network rm "$NET" >/dev/null 2>&1 || true' EXIT

# --- configs: A uses the "host:port" string form, B the {ip,port} table form ---
cat > "$WORK/peerA.lua" <<'LUA'
return {
  ["local"]  = { endpoint = { ip = "0.0.0.0", port = 58989 } },
  ["remote"] = { endpoint = "peerB:58990" },
}
LUA
cat > "$WORK/peerB.lua" <<'LUA'
return {
  ["local"]  = { endpoint = { ip = "0.0.0.0", port = 58990 } },
  ["remote"] = { endpoint = { ip = "peerA", port = 58989 } },
}
LUA
cat > "$WORK/Containerfile" <<EOF
FROM $IMAGE
COPY --chown=edge:cordoba peerA.lua /app/peerA.lua
COPY --chown=edge:cordoba peerB.lua /app/peerB.lua
EOF

$ENGINE build -t marvel:gnmi-peer-smoke "$WORK" >/dev/null

$ENGINE network exists "$NET" 2>/dev/null || $ENGINE network create "$NET" >/dev/null
$ENGINE rm -f peerA peerB >/dev/null 2>&1 || true

# Peer B — receiver (stdin closed => stays serving).
$ENGINE run -d --name peerB --network "$NET" marvel:gnmi-peer-smoke \
  /app/gnmi_peer --config=/app/peerB.lua --headless=true >/dev/null

for _ in $(seq 1 20); do
  $ENGINE logs peerB 2>&1 | grep -q 'headless config' && break
  read -t 1 </dev/null || true
done

# Peer A — sender: one multi-pair set, then EOF (auto-quits after grace).
printf 'gnmi set /smoke/leaf:5,/smoke/other:up\n' | \
  $ENGINE run -i --rm --name peerA --network "$NET" marvel:gnmi-peer-smoke \
    /app/gnmi_peer --config=/app/peerA.lua --headless=true >/dev/null 2>&1 || true

LOGS="$($ENGINE logs peerB 2>&1)"
echo "----- peerB logs -----"; echo "$LOGS"; echo "----------------------"

fail=0
check() { echo "$LOGS" | grep -qF "$1" && echo "PASS: $1" || { echo "FAIL: $1"; fail=1; }; }
check "[Set] role=ADMIN update_count=2"
check '"op":"UPDATE","path":"/smoke/leaf","val":"5"'
check '"op":"UPDATE","path":"/smoke/other","val":"up"'

[ "$fail" -eq 0 ] && echo "SMOKE TEST: PASS" || { echo "SMOKE TEST: FAIL"; exit 1; }
