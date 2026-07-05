#!/usr/bin/env bash
#
# build.sh — build the marvel image with docker or podman (whichever is present).
#
# The build context is this script's directory (hackthon/), which holds the
# Dockerfile. Produces an image (default marvel:release) containing all binaries:
# /app/app, /app/cli_app, /app/gnmi_peer, /app/vpn_server, /app/vpn_client,
# /app/openvpn_server, /app/openvpn_client.
#
# Usage:
#   ./build.sh [options]
#
# Options:
#   -e, --engine <docker|podman>  Force the container engine (default: auto)
#   -t, --image  <name[:tag]>     Image tag to build   (default: marvel:release)
#       --tests  <on|off>         Run the gtest suite during build (default: off)
#       --build-type <type>       CMAKE_BUILD_TYPE     (default: Debug)
#       --no-cache                Build without layer cache
#       --pull                    Always attempt to pull a newer base image
#       --                        Pass any following args straight to `build`
#   -h, --help                    Show this help
#
# Env:
#   ENGINE   Same as --engine.
#
# Examples:
#   ./build.sh                       # fast build, no tests
#   ./build.sh --tests on            # build and run ctest
#   ./build.sh -t marvel:dev --no-cache
set -eo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

IMAGE="marvel:release"
TESTS="off"
BUILD_TYPE="Debug"
ENGINE="${ENGINE:-}"
NO_CACHE=""
PULL=""
passthru=()

die() { echo "build.sh: $*" >&2; exit 1; }

usage() { awk 'NR==1{next} /^set -e/{exit} {sub(/^# ?/,""); print}' "$0"; }

# Prefer docker when its daemon is reachable, else podman, else whichever exists.
detect_engine() {
  if [ -n "$ENGINE" ]; then echo "$ENGINE"; return; fi
  if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then echo docker; return; fi
  if command -v podman >/dev/null 2>&1; then echo podman; return; fi
  if command -v docker >/dev/null 2>&1; then echo docker; return; fi
  echo ""
}

while [ $# -gt 0 ]; do
  case "$1" in
    -e|--engine)     ENGINE="$2"; shift 2 ;;
    -t|--image)      IMAGE="$2"; shift 2 ;;
    --tests)         TESTS="$2"; shift 2 ;;
    --build-type)    BUILD_TYPE="$2"; shift 2 ;;
    --no-cache)      NO_CACHE="--no-cache"; shift ;;
    --pull)          PULL="--pull"; shift ;;
    -h|--help)       usage; exit 0 ;;
    --)              shift; passthru=("$@"); break ;;
    *)               die "unknown option: $1 (try --help)" ;;
  esac
done

engine="$(detect_engine)"
[ -n "$engine" ] || die "no container engine found — install docker or podman"

# Dockerfile expects ON/OFF for RUN_TESTS.
case "$TESTS" in
  on|ON|true|1)   run_tests="ON" ;;
  off|OFF|false|0) run_tests="OFF" ;;
  *) die "--tests must be on|off" ;;
esac

set -x
exec "$engine" build $NO_CACHE $PULL \
  --build-arg RUN_TESTS="$run_tests" \
  --build-arg CMAKE_BUILD_TYPE="$BUILD_TYPE" \
  -t "$IMAGE" \
  -f "$here/Dockerfile" \
  "${passthru[@]}" \
  "$here"
