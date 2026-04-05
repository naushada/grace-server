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

WORKDIR /src
COPY . /src

# Allow overriding the build type at build time; default to Debug for easier troubleshooting
ARG CMAKE_BUILD_TYPE=Debug
# Optional external proto dir for third_party imports
ARG EXTERNAL_PROTO_DIR=""
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DEXTERNAL_PROTO_DIR="${EXTERNAL_PROTO_DIR}" \
 && cmake --build build -j "$(nproc)"

FROM ubuntu:22.04 AS runtime

ENV DEBIAN_FRONTEND=noninteractive
# Runtime dependencies for the built binary (libevent, nghttp2, openssl)
RUN apt-get update && apt-get install -y \
    libevent-2.1-7 \
    libnghttp2-14 \
    libprotobuf23 \
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

COPY --from=build /src/build/app/app /app/app

CMD ["/app/app"]
