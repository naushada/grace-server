---
name: device-soak
description: Run a bounded soak (default 30 min) of one dial-out stack — mgmt, grpc-tunnel, or gnmi-cli — against a real device. Reaches the device over `t3 console`, enables its endpoints, starts the stack via service.sh, captures output to a file, and summarises sessions/updates/errors. Use when asked to soak, exercise, smoke-test, or capture telemetry from a device, or to debug a dial-out that misbehaves only over time.
---

# Device soak

Six steps, one command:

| # | Step | How |
|---|------|-----|
| 1 | Reach the device | `t3 console <host> -c '<probe>'` |
| 2 | Enable endpoints | `t3 console <host> -c '<cmd>'`, one per line of `--enable-file` |
| 3 | Start the stack | `service.sh mgmt` \| `grpc-tunnel` \| `gnmi-cli` |
| 4 | Capture | → `<out-dir>/capture.log` |
| 5 | Sample | wall-clock line counts → `<out-dir>/timeline.tsv` |
| 6 | Process | `analyze.sh` → `summary.txt` + `updates.tsv` |

```bash
scripts/soak.sh --mode mgmt --host rn-147 \
  --server 10.0.0.5:58989 --enable-file ./enable.txt --duration 30m
```

Everything lands in `logs/soak-<mode>-<stamp>/`. `--dry-run` prints the plan without touching the device.

## Before you run

**The enable commands are device-specific and the script will not guess them.**
Put them in a file, one per line (`#` comments allowed); `{{server}}` expands to `--server`:

```
# enable.txt — device CLI that points the dial-out at our server
set dialout server {{server}}
set dialout enable true
```

Then `--enable-file ./enable.txt`. If the device is already configured, pass `--no-enable` and skip steps 1–2 entirely (`--host` then becomes optional).

The reachability probe defaults to `show version`; override with `--probe-cmd` if that isn't valid on the device. `t3` must be on `PATH` (or set `T3_BIN`).

## Picking a mode

**`mgmt`** — passive capture. The device dials `:58989` and opens `DialTcc.Subscribe`; every `DeviceResponse` and proactive push is teed to the capture file for the full window.

> Headless `mgmt-dialout` has **no stdin reader** (`app/src/main_app.cpp:487`) — it only runs the event loop. Commands can only be typed into the ncurses TUI, so an unattended mgmt soak observes; it does not drive. The script uses `service.sh mgmt --no-attach` because attaching with a closed stdin sends `^D` and quits the TUI.

**`grpc-tunnel`** — waits for the device to register, reads its published target out of `service.sh ps`, writes it into `docs/tunnel.lua` `listeners["9339"]`, restarts, confirms the bridge is up, then streams `service.sh subscribe <path>`. This automates the manual "grab the target, edit tunnel.lua, restart" loop that `service.sh` prints on startup.

**`gnmi-cli`** — the only mode that actively drives traffic. `gnmi_peer --headless` reads command lines from stdin, so the script subscribes to each `--path` once and then polls a `get` on every sample tick, holding stdin open for the whole run. Point it at a target with `--config <endpoint.lua>` (default `docs/endpoint.lua`; `remote = "127.0.0.1:9339"` goes through a running tunnel).

## Reading the output

`summary.txt` counts sessions, replies vs. proactive pushes, subscribe notifications, CLI exit codes, and errors. `updates.tsv` is the "processing for updates" step: one row per gNMI path with `samples`, `changes`, `first_value`, `last_value` — so a path that moved during the soak is obvious. The summary reprints just the changed ones.

`capture.log` has no timestamps (the `--log-file` sink writes bare lines), which is why `timeline.tsv` exists: it records wall-clock line counts every `--sample-interval` seconds. `analyze.sh` uses it to report idle stretches, and shouts if the device produced nothing for the entire window.

Re-run the analysis on any old capture without re-soaking:

```bash
scripts/analyze.sh logs/soak-mgmt-20260710-101500/capture.log \
                   logs/soak-mgmt-20260710-101500/timeline.tsv
```

## Notes

- The stack is torn down (`service.sh stop`) on exit, including on Ctrl-C. `--keep-up` leaves it running so you can `service.sh attach`.
- `mgmt` and `grpc-tunnel` both bind `:58989`; starting one stops the other. Don't soak both at once.
- `gnmi-cli` runs with `--network host`, which is degraded on Docker Desktop for Mac — prefer a Linux host, or reach the device through a tunnel already published on `127.0.0.1:9339`.
- A soak that ends with `0 devices connected` means step 2 didn't take: check `02-enable.log`, and confirm the device's dial-out target really points at `--server`.
