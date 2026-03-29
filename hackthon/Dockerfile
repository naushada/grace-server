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
  && rm -rf /var/lib/apt/lists/*

# Ensure protoc always enables proto3 optional
RUN printf '#!/usr/bin/env bash\nexec /usr/bin/protoc --experimental_allow_proto3_optional "$@"\n' > /usr/local/bin/protoc && \
    chmod +x /usr/local/bin/protoc

WORKDIR /src
COPY . /src

# Ensure protoc wrapper is executable when present
RUN if [ -f /src/app/idl/proto/protoc_wrapper.sh ]; then chmod +x /src/app/idl/proto/protoc_wrapper.sh; fi

# Allow overriding the build type at build time; default to Debug for easier troubleshooting
ARG CMAKE_BUILD_TYPE=Debug
# Optional external proto dir for third_party imports
ARG EXTERNAL_PROTO_DIR=""
RUN cmake -S . -B build -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE} -DEXTERNAL_PROTO_DIR="${EXTERNAL_PROTO_DIR}" \
 && cmake --build build -j "$(nproc)"

# MQTT release/2.0 compilation
WORKDIR /tmp
RUN git clone -b release/2.0 https://github.com/eclipse/mosquitto.git && \
    cd /tmp/mosquitto && \
    make install WITH_CJSON=no WITH_DOCS=no&&\
    ldconfig



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
#COPY --from=build /src/build/service/service /app/service
COPY --from=build /src/build/app_main/app_main /app/app_main
COPY --from=build /src/mqtt/mosquitto.conf /app/mosquitto.conf
COPY --from=build /tmp/mosquitto/lib/libmosquitto.so.1 /app/libmosquitto.so.1
COPY --from=build /tmp/mosquitto/src/mosquitto /app/mosquitto
COPY --from=build /tmp/mosquitto/client/mosquitto_pub /app/mosquitto_pub
USER root
COPY --from=build /tmp/mosquitto/lib/libmosquitto.so.1 /usr/lib/libmosquitto.so.1
RUN ln -s /usr/lib/libmosquitto.so.1 /usr/lib/libmosquitto.so
RUN ldconfig




CMD ["/app/app_main"]
