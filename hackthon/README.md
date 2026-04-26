# hackthon — gNMI gRPC Server over nghttp2

A C++20 gNMI target server built on libevent (I/O) + nghttp2 (HTTP/2) with a
hand-rolled gRPC framing layer.  No `libgrpc++` dependency — gRPC is
implemented directly over HTTP/2.

---

## Full Receive Pipeline

```
libevent (socket event)
  └─ client_read_cb()                     [app/src/framework.cpp]
       ├─ dry_run=true  → handle_read()   returns 0 ("can handle")
       └─ dry_run=false → handle_read()   [app/src/client_app.cpp]
            └─ grpc_session::recv()       [app/src/grpc_session.cpp]
                 └─ http2_session::recv() [app/src/http2.cpp  /  nghttp2]
                      └─ on_request()     fires when a complete HTTP/2 stream arrives
                           ├─ validates Content-Type: application/grpc[+proto]
                           ├─ grpc_session::decode_frame()   strips 5-byte length prefix
                           └─ registered unary handler  (keyed by URI path)
                                ├─ /gnmi.gNMI/Capabilities → CapabilityRequest  → CapabilityResponse
                                ├─ /gnmi.gNMI/Get          → GetRequest         → GetResponse
                                ├─ /gnmi.gNMI/Set          → SetRequest         → SetResponse
                                └─ /gnmi.gNMI/Subscribe    → UNIMPLEMENTED (grpc-status: 12)
                                     └─ send_unary_response()
                                          ├─ encode_frame()                wraps serialised proto bytes in 5-byte prefix
                                          ├─ http2_session::submit_response()  HEADERS (:status 200) + DATA (with trailer_mode)
                                          ├─ http2_session::submit_trailer()   trailing HEADERS (grpc-status)
                                          └─ raw_tx callback
                                               └─ connected_client::tx()   → libevent send buffer → socket
```

---

## gRPC Wire Format (implemented without libgrpc++)

| Layer       | What we do |
|-------------|------------|
| HTTP/2      | nghttp2 session (server-side); handles SETTINGS, WINDOW_UPDATE, PING, stream lifecycle |
| gRPC framing | 5-byte length-prefix: `[0x00][4-byte big-endian length][protobuf bytes]` |
| Trailers    | Trailing HEADERS frame carrying `grpc-status` (and optionally `grpc-message`) sent after DATA with `NO_END_STREAM` |
| Protobuf    | OpenConfig gNMI proto compiled by `protoc --cpp_out`; no `grpc_cpp_plugin` needed |

---

## Project Layout

```
app/
  inc/
    framework.hpp       libevent base, evt_io, server/client base classes
    http2.hpp           http2_session — transport-agnostic nghttp2 wrapper
    grpc_session.hpp    grpc_session  — gRPC framing + unary RPC dispatch
    client_app.hpp      connected_client — per-connection object
    server_app.hpp      server — accepts connections, owns connected_client map
    fs_app.hpp          filesystem watcher (inotify / Lua command loader)
    lua_engine.hpp      Lua 5.4 scripting engine
  src/
    http2.cpp
    grpc_session.cpp
    client_app.cpp      gNMI RPC handlers registered here
    server_app.cpp
    framework.cpp
    fs_app.cpp
    lua_engine.cpp
    main_app.cpp
  idl/
    gnmi/gnmi.proto
    gnmi_ext/gnmi_ext.proto
    collector/collector.proto
    target/target.proto
  test/
    http2_test.cpp
    grpc_session_test.cpp
    client_app_test.cpp
    server_app_test.cpp
    framework_test.cpp
    fs_app_test.cpp
    lua_engine_test.cpp
  cli/                  readline-based CLI (separate CMake target)
```

---

## Build

```bash
docker build -t hackthon .
```

The Dockerfile compiles everything, runs all gtests (`ctest --output-on-failure`),
and produces a minimal runtime image.  To skip tests:

```bash
docker build --build-arg RUN_TESTS=OFF -t hackthon .
```

### Dependencies (all resolved inside the container)

| Package              | Used by |
|----------------------|---------|
| `libnghttp2-dev`     | `http2_session`, `grpc_session` |
| `libprotobuf-dev`    | generated gNMI proto classes |
| `protobuf-compiler`  | `protoc --cpp_out` at build time |
| `libevent-dev`       | `evt_io`, `evt_base` |
| `libssl-dev`         | OpenSSL (linked transitively by libevent) |
| `liblua5.4-dev`      | `lua_engine` |
| `libgtest-dev`       | unit tests |

---

## CLI — gNMI Operations

The `cli_app` binary provides a readline REPL that can send gNMI operations
to a peer device.  Four Lua command files are shipped in `app/command/`:

| File | Command | gNMI operation |
|------|---------|----------------|
| `gnmi_get.lua`     | `gnmi_get`     | `GetRequest` |
| `gnmi_update.lua`  | `gnmi_update`  | `SetRequest.update[]` |
| `gnmi_replace.lua` | `gnmi_replace` | `SetRequest.replace[]` |
| `gnmi_delete.lua`  | `gnmi_delete`  | `SetRequest.delete[]` |

### Arguments (all commands)

| Key | Default | Description |
|-----|---------|-------------|
| `target` | `127.0.0.1` | IP address or hostname of the peer gNMI device |
| `port` | `9339` | TCP port |
| `prefix` | `/` | Common YANG path prefix shared by all paths in the request |
| `path` | — | Specific leaf or subtree relative to prefix |
| `value` | — | New value (SET/UPDATE/REPLACE only); plain string or JSON object |
| `encoding` | `JSON` | Wire encoding: `JSON`, `JSON_IETF`, `PROTO` |

### Path format

Both `prefix` and `path` use the YANG instance-identifier syntax:
```
/module:container/list[key=value]/leaf
```
Key predicates like `[name=eth0]` are parsed and encoded into
`gnmi.PathElem.key` maps.

### Examples

```
# Fetch the operational status of interface eth0
Marvel> gnmi_get target=192.168.1.1 prefix=/interfaces/interface[name=eth0] path=state/oper-status

# Update the description of interface eth0
Marvel> gnmi_update target=192.168.1.1 prefix=/interfaces/interface[name=eth0] path=config/description value=uplink-to-spine

# Replace the entire config subtree of eth0
Marvel> gnmi_replace target=192.168.1.1 prefix=/interfaces path=/interface[name=eth0]/config value={"description":"new-uplink","enabled":true}

# Delete interface eth0 from the configuration
Marvel> gnmi_delete target=192.168.1.1 prefix=/interfaces path=/interface[name=eth0]
```

### How it works (CLI → peer device)

```
readline REPL
  └─ process_command()                  detect cmd_name == gnmi_*
       └─ handle_gnmi_get/update/...()
            ├─ parse_yang_path()        "/prefix" + "path" → gnmi::Path
            ├─ build gnmi proto         gnmi::GetRequest / SetRequest
            ├─ SerializeToString()      raw protobuf bytes
            └─ gnmi_client::call()      [app/src/gnmi_client.cpp]
                 ├─ connect_tcp()       blocking POSIX connect() to target:port
                 ├─ http2_session       client-side nghttp2 session
                 │    └─ send connection preface + SETTINGS
                 ├─ grpc_session::encode_frame()   5-byte length prefix
                 ├─ http2_session::submit_request  POST /gnmi.gNMI/<Method>
                 └─ blocking recv loop
                      └─ grpc_session::decode_frame()  strip 5-byte prefix
                           └─ print gnmi proto in text format
```

---

## Adding a New gRPC Handler

Register a unary handler in `connected_client::register_gnmi_handlers()`
(`app/src/client_app.cpp`):

```cpp
m_grpc->register_unary("/mypackage.MyService/MyMethod",
    [](const std::string &req_pb) -> std::pair<int, std::string> {
        mypackage::MyRequest req;
        if (!req.ParseFromString(req_pb))
            return {3, ""};  // INVALID_ARGUMENT

        mypackage::MyResponse resp;
        // ... fill resp ...
        std::string out;
        resp.SerializeToString(&out);
        return {0, out};  // OK
    });
```

gRPC status codes: `0` OK, `3` INVALID_ARGUMENT, `5` NOT_FOUND, `12` UNIMPLEMENTED, `13` INTERNAL.
