#!/usr/bin/env bash
#
# analyze.sh <capture.log> [timeline.tsv]
#
# Turns a soak capture into a summary + an updates table. Parses the line shapes
# emitted by app/src/client_app.cpp (ANSI already stripped by the --log-file /
# --out-file sink):
#
#   [mgmt] session #1 opened|closed
#   ▸ role(hostname) connected  ·  device RN-147
#   [mgmt] reply 'show version'  rpc=r-8f3a  dev=RN-147
#   [mgmt] push  rpc=…                       (unsolicited / proactive)
#   [mgmt] unparsable DeviceResponse (N bytes)
#       exit=0  12ms  |  [stderr] …          (CliResponse)
#       /system/state/hostname = "rn-147"    (GetResponse update)
#       [notif #3, updates:2, del:1] {…}     (SubscribeResponse)
#   [reg] +target 'X' | -target 'X' (disconnected)
#
# Writes updates.tsv beside the capture; prints the summary to stdout.
set -euo pipefail

CAP="${1:?usage: analyze.sh <capture.log> [timeline.tsv]}"
TIMELINE="${2:-}"
OUT_DIR="$(cd "$(dirname "$CAP")" && pwd)"
UPDATES="$OUT_DIR/updates.tsv"

[ -f "$CAP" ] || { echo "analyze: no capture at $CAP" >&2; exit 1; }

awk -v updates="$UPDATES" '
function trim(s) { sub(/^[ \t]+/, "", s); sub(/[ \t]+$/, "", s); return s }

{ total++ }

/^\[mgmt\] session #/          { if (/opened/) s_open++; else if (/closed/) s_closed++; next }
/^\[mgmt\] unparsable/         { unparsable++; errors++; next }
/^\[mgmt\] reply /             { replies++; next }
/^\[mgmt\] push/               { pushes++; next }
/^\[stderr\]/                  { stderr_lines++; errors++; next }
/^\[reg\] \+target/            { reg++; next }
/^\[reg\] -target/             { dereg++; if (/disconnected/) disc++; next }
/^\[tun\] Tunnel stream opened/ { tun++; next }
index($0, "connected") && index($0, "\xe2\x96\xb8") { devices++; next }   # ▸ … connected

# CliResponse status line:  "    exit=0  (timeout)  123ms"
/exit=/ && /ms$/ {
  if (match($0, /exit=[0-9]+/)) {
    code = substr($0, RSTART + 5, RLENGTH - 5) + 0
    if (code == 0) cli_ok++; else { cli_fail++; errors++ }
  }
  if (index($0, "(timeout)"))   { timeouts++; errors++ }
  if (index($0, "(truncated)")) truncated++
  next
}

# SubscribeResponse:  "    [notif #7, updates:2, del:1] {json}"
/\[notif #/ {
  notifs++
  if (match($0, /updates:[0-9]+/)) upd_total += substr($0, RSTART + 8, RLENGTH - 8) + 0
  if (match($0, /del:[0-9]+/))     del_total += substr($0, RSTART + 4, RLENGTH - 4) + 0
  next
}

# GetResponse update:  "    /system/state/hostname = "rn-147""
{
  line = trim($0)
  if (substr(line, 1, 1) != "/") next
  i = index(line, " = ")
  if (i == 0) next
  p = substr(line, 1, i - 1)
  v = substr(line, i + 3)
  samples[p]++
  if (!(p in first)) { first[p] = v; order[++npaths] = p }
  else if (last[p] != v) changes[p]++
  last[p] = v
  upd_lines++
}

END {
  printf "capture lines      : %d\n", total
  printf "devices connected  : %d\n", devices + 0
  printf "sessions           : %d opened, %d closed\n", s_open + 0, s_closed + 0
  if (reg || dereg)
    printf "tunnel targets     : %d registered, %d deregistered (%d disconnects), %d streams\n",
           reg + 0, dereg + 0, disc + 0, tun + 0
  printf "responses          : %d replies, %d proactive pushes\n", replies + 0, pushes + 0
  if (cli_ok || cli_fail)
    printf "cli results        : %d ok, %d non-zero exit, %d timeouts, %d truncated\n",
           cli_ok + 0, cli_fail + 0, timeouts + 0, truncated + 0
  if (notifs)
    printf "subscribe notifs   : %d (%d updates, %d deletes)\n", notifs, upd_total + 0, del_total + 0
  printf "gnmi update lines  : %d across %d distinct paths\n", upd_lines + 0, npaths + 0
  printf "errors             : %d (%d unparsable, %d stderr, %d cli-fail, %d timeout)\n",
         errors + 0, unparsable + 0, stderr_lines + 0, cli_fail + 0, timeouts + 0

  if (npaths) {
    printf "path\tsamples\tchanges\tfirst_value\tlast_value\n" > updates
    changed = 0
    for (n = 1; n <= npaths; n++) {
      p = order[n]
      printf "%s\t%d\t%d\t%s\t%s\n", p, samples[p], changes[p] + 0, first[p], last[p] > updates
      if (changes[p] > 0) changed++
    }
    close(updates)
    printf "paths that changed : %d of %d\n", changed, npaths
    if (changed) {
      print  "\nchanged values:"
      for (n = 1; n <= npaths; n++) {
        p = order[n]
        if (changes[p] > 0)
          printf "  %-52s %s  ->  %s   (%dx)\n", p, first[p], last[p], changes[p]
      }
    }
  }
  if (!total) print "\n(empty capture — the device never sent anything)"
}
' "$CAP"

# ── liveness over wall-clock, from the sampler ───────────────────────────────
if [ -n "$TIMELINE" ] && [ -f "$TIMELINE" ]; then
  awk -F'\t' '
    NR == 1 { next }
    {
      n++; elapsed = $2; delta = $4 + 0
      if (delta == 0) { stalls++; run++; if (run > longest) longest = run } else run = 0
    }
    END {
      if (!n) { print "\ntimeline           : no samples"; exit }
      printf "\nsoak window        : %ds across %d samples\n", elapsed, n
      printf "idle samples       : %d of %d (longest silent stretch: %d samples)\n", stalls + 0, n, longest + 0
      if (stalls == n) print "warning            : the device produced NOTHING for the whole run"
    }
  ' "$TIMELINE"
fi

[ -f "$UPDATES" ] && echo && echo "updates table      : $UPDATES"
exit 0
