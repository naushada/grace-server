FROM ubuntu:22.04 AS build

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    git \
    pkg-config \
    libevent-dev \
    libnghttp2-dev \
    libssl-dev \
    zlib1g-dev \
    protobuf-compiler \
    libprotobuf-dev \
    libgtest-dev \
    liblua5.4-dev \
    libreadline-dev \
  && rm -rf /var/lib/apt/lists/*
# Note: grpc_session is implemented directly over nghttp2 (no libgrpc++ needed).
# libnghttp2-dev above covers both http2_session and grpc_session.

WORKDIR /src
COPY . /src

# Allow overriding the build type at build time; default to Debug for easier troubleshooting
ARG CMAKE_BUILD_TYPE=Debug
# Optional external proto dir for third_party imports
ARG EXTERNAL_PROTO_DIR=""
# Toggle unit tests — ON by default so the image fails to build if the
# gtest suites in app/test and app/cli/test regress. Pass
# `--build-arg RUN_TESTS=OFF` to skip.
ARG RUN_TESTS=ON
RUN cmake -S . -B build \
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} \
      -DEXTERNAL_PROTO_DIR="${EXTERNAL_PROTO_DIR}" \
      -DBUILD_TESTING=ON \
 && cmake --build build -j "$(nproc)" \
 && if [ "$RUN_TESTS" = "ON" ]; then \
      ctest --test-dir build --output-on-failure; \
    fi

FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
# Runtime dependencies for the built binaries.
#   - libevent/libnghttp2/libssl/zlib/libprotobuf: used by `app`
#   - libreadline8:                               used by `cli_app`
#   - liblua5.4-0:                                shared by both (lua_engine)
#   - gdb:                                        kept for in-container debugging
RUN apt-get update && apt-get install -y \
    libevent-2.1-7 \
    libnghttp2-14 \
    libprotobuf23 \
    libreadline8 \
    libssl3 \
    zlib1g \
    gdb \
    liblua5.4-0 \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /app
# Create a user group 'cordoba'
RUN groupadd cordoba
# Create a user 'edge' under 'cordoba'
RUN adduser edge
# Chown all the files to the app user.
RUN chown -R edge:cordoba /app
# Switch to 'edge'
USER edge

COPY --from=build --chown=edge:cordoba /src/build/app/app /app/app
COPY --from=build --chown=edge:cordoba /src/build/app/cli/cli_app /app/cli_app

# fs_app in readline.cpp / fs_app.cpp watches /app/command for .lua command
# definitions. Ship the sample StartDownlinkConnectionsTestRequest.lua so the
# CLI has something to auto-complete out of the box. Operators can drop
# additional .lua files here at runtime — inotify picks them up live.
COPY --from=build --chown=edge:cordoba /src/app/command/ /app/command/
COPY --from=build --chown=edge:cordoba /src/app/openvpn/command/ /app/command/

# Default: server mode. Override CMD at runtime for client mode:
#   docker run ... /app/app --mode=client --server=<ip> --tun=tun0 --status=/run/vpn_status.lua

CMD ["/app/app"]
