#!/usr/bin/env bash
#
# soak.sh — drive one of the three dial-out stacks against a real device for a
# bounded run, capture everything to a file, and summarise it.
#
#   1. reach the device       t3 console <host> -c '<probe>'
#   2. enable the endpoints   t3 console <host> -c '<cmd>' … (from --enable-file)
#   3. start the stack        ./service.sh mgmt | grpc-tunnel | gnmi-cli
#   4. capture                stdout / --out-file -> <out-dir>/capture.log
#   5. sample                 wall-clock line counts -> <out-dir>/timeline.tsv
#   6. analyse                analyze.sh -> summary.txt + updates.tsv
#
# Usage:
#   soak.sh --mode mgmt|grpc-tunnel|gnmi-cli --host <device> [options]
#
#   --server <ip[:port]>    dial-out server the device should reach; substituted
#                           for {{server}} in the enable commands
#   --duration <30m|1800s>  default 30m
#   --out-dir <dir>         default logs/soak-<mode>-<stamp>
#   --enable-file <path>    one device command per line ('#' comments). Required
#                           unless --no-enable — the commands are device-specific
#                           and this script will not guess them.
#   --enable-cmd <cmd>      repeatable; appended after --enable-file's commands
#   --no-enable             skip steps 1-2 (device already set up)
#   --probe-cmd <cmd>       reachability probe, default 'show version'
#   --path <gnmi-path>      repeatable; what to subscribe/poll (grpc-tunnel,
#                           gnmi-cli). Default /system/state
#   --config <endpoint.lua> gnmi-cli only; default docs/endpoint.lua
#   --sample-interval <s>   default 30
#   --keep-up               leave the stack running after the soak
#   --dry-run               print what would run, touch nothing
#
# Env: T3_BIN (default t3) · SERVICE_SH · ENGINE=docker|podman
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../../.." && pwd)"          # …/hackthon
SERVICE="${SERVICE_SH:-$ROOT/service.sh}"
T3="${T3_BIN:-t3}"

MODE="" HOST="" SERVER="" DURATION=1800 OUT_DIR="" PROBE="show version"
ENABLE_FILE="" DO_ENABLE=1 SAMPLE=30 KEEP_UP=0 DRY=0 CFG=""
ENABLE_CMDS=() PATHS=()

die() { echo "soak: $*" >&2; exit 1; }
say() { printf '\033[1;36m[soak]\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m[soak]\033[0m %s\n' "$*" >&2; }
strip_ansi() { sed $'s/\033\\[[0-9;]*m//g'; }

# 30m / 90s / 1h / bare seconds -> seconds
parse_duration() {
  case "$1" in
    *h) echo $(( ${1%h} * 3600 )) ;;
    *m) echo $(( ${1%m} * 60 )) ;;
    *s) echo "${1%s}" ;;
    *)  echo "$1" ;;
  esac
}

while [ $# -gt 0 ]; do
  case "$1" in
    --mode)            MODE="$2"; shift 2 ;;
    --host)            HOST="$2"; shift 2 ;;
    --server)          SERVER="$2"; shift 2 ;;
    --duration)        DURATION="$(parse_duration "$2")"; shift 2 ;;
    --out-dir)         OUT_DIR="$2"; shift 2 ;;
    --enable-file)     ENABLE_FILE="$2"; shift 2 ;;
    --enable-cmd)      ENABLE_CMDS+=("$2"); shift 2 ;;
    --no-enable)       DO_ENABLE=0; shift ;;
    --probe-cmd)       PROBE="$2"; shift 2 ;;
    --path)            PATHS+=("$2"); shift 2 ;;
    --config)          CFG="$2"; shift 2 ;;
    --sample-interval) SAMPLE="$2"; shift 2 ;;
    --keep-up)         KEEP_UP=1; shift ;;
    --dry-run)         DRY=1; shift ;;
    -h|--help)         awk 'NR==1{next} /^set -e/{exit} {sub(/^# ?/,""); print}' "$0"; exit 0 ;;
    *) die "unknown option '$1' (--help)" ;;
  esac
done

case "$MODE" in
  mgmt|grpc-tunnel|gnmi-cli) ;;
  "") die "--mode is required (mgmt|grpc-tunnel|gnmi-cli)" ;;
  *)  die "unknown --mode '$MODE'" ;;
esac
[ "$DO_ENABLE" -eq 1 ] && [ -z "$HOST" ] && die "--host is required (or pass --no-enable)"
[ -x "$SERVICE" ] || die "service.sh not found/executable: $SERVICE"
[ "${#PATHS[@]}" -eq 0 ] && PATHS=(/system/state)

STAMP="$(date +%Y%m%d-%H%M%S)"
OUT_DIR="${OUT_DIR:-$ROOT/logs/soak-$MODE-$STAMP}"
CAP="$OUT_DIR/capture.log"
TIMELINE="$OUT_DIR/timeline.tsv"

# ── engine (mirrors service.sh's detection, for health checks only) ──────────
engine() {
  case "${ENGINE:-}" in docker|podman) echo "$ENGINE"; return ;; esac
  command -v docker >/dev/null 2>&1 && { echo docker; return; }
  command -v podman >/dev/null 2>&1 && { echo podman; return; }
  echo docker
}
ENG="$(engine)"

run() {   # run, or just show it under --dry-run
  if [ "$DRY" -eq 1 ]; then printf '  + %s\n' "$*"; return 0; fi
  "$@"
}

# ── step 1: reach the device ─────────────────────────────────────────────────
step_reach() {
  say "1/6 reaching $HOST via '$T3 console' …"
  if [ "$DRY" -eq 1 ]; then
    command -v "$T3" >/dev/null 2>&1 || warn "'$T3' not on PATH — a real run would fail here"
    printf '  + %s console %s -c %q\n' "$T3" "$HOST" "$PROBE"; return 0
  fi
  command -v "$T3" >/dev/null 2>&1 || die "'$T3' not on PATH (set T3_BIN, or --no-enable)"
  if ! "$T3" console "$HOST" -c "$PROBE" >"$OUT_DIR/01-reach.log" 2>&1; then
    warn "probe failed — see $OUT_DIR/01-reach.log"
    die "cannot reach $HOST (try a different --probe-cmd)"
  fi
  say "    reachable (probe output -> 01-reach.log)"
}

# ── step 2: enable the endpoints ─────────────────────────────────────────────
# The commands are device-specific. They come from --enable-file / --enable-cmd;
# {{server}} expands to --server. We refuse to guess.
step_enable() {
  local cmds=() line
  if [ -n "$ENABLE_FILE" ]; then
    [ -f "$ENABLE_FILE" ] || die "--enable-file not found: $ENABLE_FILE"
    while IFS= read -r line || [ -n "$line" ]; do
      case "$line" in ''|'#'*) continue ;; esac
      cmds+=("$line")
    done < "$ENABLE_FILE"
  fi
  cmds+=(${ENABLE_CMDS[@]+"${ENABLE_CMDS[@]}"})
  [ "${#cmds[@]}" -eq 0 ] && die "no enable commands: pass --enable-file/--enable-cmd, or --no-enable"

  say "2/6 enabling endpoints on $HOST (${#cmds[@]} command(s)) …"
  local c
  for c in "${cmds[@]}"; do
    case "$c" in *'{{server}}'*)
      [ -n "$SERVER" ] || die "command uses {{server}} but --server was not given"
      c="${c//\{\{server\}\}/$SERVER}" ;;
    esac
    if [ "$DRY" -eq 1 ]; then printf '  + %s console %s -c %q\n' "$T3" "$HOST" "$c"; continue; fi
    printf '\n$ %s\n' "$c" >> "$OUT_DIR/02-enable.log"
    if ! "$T3" console "$HOST" -c "$c" >>"$OUT_DIR/02-enable.log" 2>&1; then
      die "enable command failed: '$c' (see $OUT_DIR/02-enable.log)"
    fi
    say "    ok: $c"
  done
}

# ── step 3+4: start the stack, capture to $CAP ───────────────────────────────
CAP_PID=""

start_mgmt() {
  # Passive capture: headless mgmt has no stdin reader, and attaching with a
  # closed stdin sends ^D (quits the TUI). --no-attach leaves mgmt-svc detached
  # writing every DeviceResponse + proactive push to --out-file.
  say "3/6 starting mgmt dial-out (detached, :58989) …"
  run "$SERVICE" mgmt --no-attach --out-file "$CAP"
}

# Read the device's published target out of `service.sh ps`, patch
# docs/tunnel.lua listeners["9339"], restart, and confirm the bridge is up.
tunnel_map_target() {
  local lua="$ROOT/docs/tunnel.lua" out target i
  for i in $(seq 1 24); do            # ~2 min for the device to dial in
    out="$("$SERVICE" ps 2>&1 | strip_ansi || true)"
    printf '%s\n' "$out" | grep -q 'tunnel UP' && { say "    tunnel UP"; return 0; }
    target="$(printf '%s\n' "$out" | awk '/^ *registered:/{print $2; exit}')"
    if [ -n "$target" ] && [ "$target" != "<none>" ]; then
      case "$target" in *'"'*|*'&'*|*'\'*) die "refusing to write odd target: $target" ;; esac
      say "    mapping :9339 -> '$target' in docs/tunnel.lua"
      awk -v t="$target" '
        /\["9339"\][ \t]*=/ { sub(/=[ \t]*"[^"]*"/, "= \"" t "\"") } { print }
      ' "$lua" > "$lua.soak" && mv "$lua.soak" "$lua"
      "$SERVICE" restart >/dev/null 2>&1 || true
      sleep 3
      continue
    fi
    sleep 5
  done
  return 1
}

# gnmi_cmd splits a subscribe path list on ',' (app/peer/src/gnmi_cmd.cpp).
join_paths() { local IFS=','; printf '%s' "${PATHS[*]}"; }

start_tunnel() {
  say "3/6 starting grpc-tunnel stack …"
  run "$SERVICE" grpc-tunnel
  if [ "$DRY" -eq 1 ]; then
    printf '  + <wait for [reg] +target, patch tunnel.lua listeners["9339"], restart>\n'
    printf '  + %s subscribe %s > %s\n' "$SERVICE" "$(join_paths)" "$CAP"
    return 0
  fi
  tunnel_map_target || die "no device registered on :58989 after 2m — check step 2 and the device's dial-out target"
  say "4/6 subscribing (${PATHS[*]}) -> capture.log"
  ( "$SERVICE" subscribe "$(join_paths)" >"$CAP" 2>&1 ) & CAP_PID=$!
}

# Command stream for the headless peer: subscribe once, then poll a get on every
# sample tick. Holding stdin open for the whole run is what keeps the peer alive.
gnmi_cmd_stream() {
  local end=$(( $(date +%s) + DURATION )) p
  for p in "${PATHS[@]}"; do printf 'gnmi subscribe %s\n' "$p"; done
  while [ "$(date +%s)" -lt "$end" ]; do
    sleep "$SAMPLE"
    for p in "${PATHS[@]}"; do printf 'gnmi get %s\n' "$p"; done
  done
}

start_gnmi_cli() {
  say "3/6 starting headless gnmi-cli …"
  local args=(gnmi-cli --headless)
  [ -n "$CFG" ] && args+=(--config "$CFG")
  if [ "$DRY" -eq 1 ]; then
    printf '  + <subscribe %s + poll get every %ss> | %s %s > %s\n' \
      "${PATHS[*]}" "$SAMPLE" "$SERVICE" "${args[*]}" "$CAP"
    return 0
  fi
  say "4/6 driving ${PATHS[*]} -> capture.log"
  ( gnmi_cmd_stream | "$SERVICE" "${args[@]}" >"$CAP" 2>&1 ) & CAP_PID=$!
}

# ── health, per mode ─────────────────────────────────────────────────────────
still_alive() {
  case "$MODE" in
    mgmt) [ -n "$("$ENG" ps -q --filter name=mgmt-svc 2>/dev/null)" ] ;;
    *)    [ -n "$CAP_PID" ] && kill -0 "$CAP_PID" 2>/dev/null ;;
  esac
}

# ── step 5: sample for DURATION ──────────────────────────────────────────────
step_sample() {
  say "5/6 soaking for ${DURATION}s (sample every ${SAMPLE}s) — Ctrl-C to stop early"
  printf 'epoch\telapsed\tlines\tdelta\n' > "$TIMELINE"
  local start now prev=0 lines delta end
  start="$(date +%s)"; end=$(( start + DURATION ))
  while :; do
    sleep "$SAMPLE"
    now="$(date +%s)"
    lines=$( [ -f "$CAP" ] && wc -l < "$CAP" || echo 0 )
    lines=$(( lines + 0 ))
    delta=$(( lines - prev )); prev="$lines"
    printf '%s\t%s\t%s\t%s\n' "$now" "$(( now - start ))" "$lines" "$delta" >> "$TIMELINE"
    printf '\r  %4ss elapsed · %6s lines (+%s)   ' "$(( now - start ))" "$lines" "$delta"
    if ! still_alive; then echo; warn "capture died early at $(( now - start ))s"; break; fi
    [ "$now" -ge "$end" ] && { echo; break; }
  done
}

cleanup() {
  local rc=$?
  [ -n "$CAP_PID" ] && kill "$CAP_PID" 2>/dev/null || true
  if [ "$KEEP_UP" -eq 0 ] && [ "$DRY" -eq 0 ]; then
    say "stopping the stack"
    "$SERVICE" stop >/dev/null 2>&1 || true
  fi
  exit $rc
}

# ── main ─────────────────────────────────────────────────────────────────────
[ "$DRY" -eq 0 ] && mkdir -p "$OUT_DIR"
say "mode=$MODE host=${HOST:-<none>} duration=${DURATION}s out=$OUT_DIR"

if [ "$DO_ENABLE" -eq 1 ]; then step_reach; step_enable; else say "1-2/6 skipped (--no-enable)"; fi

trap cleanup EXIT INT TERM
case "$MODE" in
  mgmt)        start_mgmt ;;
  grpc-tunnel) start_tunnel ;;
  gnmi-cli)    start_gnmi_cli ;;
esac

if [ "$DRY" -eq 1 ]; then
  printf '  + <sample every %ss for %ss>\n  + analyze.sh %s\n' "$SAMPLE" "$DURATION" "$CAP"
  say "dry run — nothing executed"; trap - EXIT; exit 0
fi

[ "$MODE" = mgmt ] && say "4/6 capturing responses + proactive pushes -> capture.log"
step_sample

say "6/6 analysing"
"$HERE/analyze.sh" "$CAP" "$TIMELINE" | tee "$OUT_DIR/summary.txt"
say "artifacts in $OUT_DIR"
