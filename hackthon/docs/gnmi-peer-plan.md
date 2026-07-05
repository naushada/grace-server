# gnmi_peer — Resume/Implementation Plan

Status: **IMPLEMENTED and build-validated** (2026-07-05). All files below were
created; `podman build --build-arg RUN_TESTS=OFF` compiled and linked
`gnmi_peer` cleanly (no warnings from the new code under -Wall -Wextra) and the
binary ships to `/app/gnmi_peer` in the image. This doc is kept as the design
record. Original plan text follows unchanged.

## Goal (user requirements)

A new interactive peer-to-peer gNMI tool:

1. Issue `gnmi set <xpath>:value[,xpath:value]` — one command, multiple updates
   in a single `SetRequest` (one `update[]` per comma-separated `xpath:value`
   pair).
2. A **Lua config file** provides the gRPC server IP & port.
3. The tool also **runs a gRPC server** at a configured IP:port. Config keys:
   `local.endpoint.ip` + `local.endpoint.port`, and `remote.endpoint.ip` +
   `remote.endpoint.port`; each endpoint may alternatively be a single
   `FQDN:port` string (`local.endpoint = "host:port"`).
4. **Two-pane terminal**: top window issues `gnmi set/get`; bottom window
   displays updates received from the remote endpoint.

## Approved design decisions (via AskUserQuestion)

- **UI**: ncurses split windows (top input line + bottom scrolling pane).
  Replaces readline for this new binary (does NOT touch existing `cli_app`).
- **Update source**: remote peer pushes gNMI `Set` to OUR local gNMI server;
  each received op is rendered in the bottom pane (reuses existing Set handler).
  Peer-to-peer: both sides run this tool.
- **Transport**: direct gRPC over HTTP/2 via `gnmi_client::push_async()` to
  `remote.endpoint` ip:port. No MQTT/VPN.

## Key codebase facts (already verified)

- Repo root of app is `hackthon/`. Build: `docker build -t marvel:release hackthon/`.
- `gnmi_client` (`app/src/gnmi_client.cpp`, hdr `app/inc/gnmi_client.hpp`):
  - `gnmi_client::call(host,port,rpc_path,request_pb,tls={})` — blocking, phased
    (only outside `event_base_dispatch`).
  - `gnmi_client::push_async(host,port,rpc_path,request_pb,tls={},on_done)` —
    non-blocking, safe INSIDE the running loop; `on_done(const response&)` fires
    once. `response{int grpc_status; string grpc_message; string body_pb; ok();}`.
- `server` (`app/inc/server_app.hpp`, `app/src/server_app.cpp`): listens on
  host:port, ctor `server(host, port, const tls_config&)`. On accept it builds a
  `connected_client` which calls `register_gnmi_handlers()`.
- `connected_client::register_gnmi_handlers()` (`app/src/client_app.cpp`)
  implements unary Get / Set / Capabilities; Subscribe returns UNIMPLEMENTED(12).
  Set enforces RBAC: requires `req.prefix().target() == "ADMIN"` else status 7.
- libevent/ncurses integration: `evt_io(evutil_socket_t fd, rawfd_tag{})` ctor
  arms `EV_READ|EV_PERSIST` and dispatches `handle_read(fd, "", false)` on
  readable (see `app/src/framework.cpp` `raw_read_cb`). Use this to wire
  `STDIN_FILENO` into the shared loop; drain ncurses keys non-blocking (nodelay).
- Single shared event base: `evt_base::instance().get()`. Loop: `run_evt_loop{}()`.
- Lua config parsing: `lua_file` (`app/inc/lua_engine.hpp`,
  `app/src/lua_engine.cpp`). `process_create_luafile(path)` runs the file and
  stores the returned table in `m_commands[path]` (keyed by the exact path
  string). `commands()` returns `const map<string, table_type>&`.
  - `table_type.members` is `map<string, entry_type>`.
  - `entry_type = variant<value_type, vector<value_type>,
     shared_ptr<table_type>, vector<shared_ptr<table_type>>>`.
  - `value_type = variant<nullptr_t, string, bool, uint32_t, int32_t, double>`.
  - `extract_value`: Lua integer -> `int32_t`; number -> `double`; string ->
    `string`; bool -> `bool`. So a port literal `58989` arrives as `int32_t`.
    Note: `local`/`remote` are Lua keywords → config must use `["local"]` /
    `["remote"]` table keys.
- Proto accessors (from `app/idl/gnmi/gnmi.proto`):
  - `gnmi::Path{ repeated PathElem elem; string target; ... }`
  - `gnmi::PathElem{ string name; map<string,string> key; }`
  - `gnmi::TypedValue` oneof: `string_val`(1), `int_val`(2,int64),
    `uint_val`(3,uint64), `bool_val`(4), `double_val`(14), `json_val`(10,bytes),
    `json_ietf_val`(11,bytes), `ascii_val`(12). Case enum e.g.
    `TypedValue::kStringVal`; discriminator `value_case()`.
  - `SetRequest{ Path prefix; repeated Update update; repeated Update replace;
     repeated Path delete_; }`; `Update{ Path path; TypedValue val; }`.
  - `GetRequest{ Path prefix; repeated Path path; Encoding encoding; }`.
- Existing YANG-path helpers to mirror live in `app/cli/src/readline.cpp`:
  `parse_yang_path`, `set_typed_value`, `parse_encoding` (copy/refactor into the
  shared header below).
- Build wiring: `app/CMakeLists.txt` builds `gnmi_proto` (STATIC) + `app`
  (globs `src/*.cpp`, which includes `main_app.cpp`'s `main`). The CLI is a
  separate target in `app/cli/CMakeLists.txt` that links selected `../src/*.cpp`.
  New binary follows the CLI pattern: own subdir, link shared source files +
  `gnmi_proto` + ncurses.

## Files to create / change

### 1. `app/inc/gnmi_util.hpp` (NEW, header-only, inline fns)
Shared so both the Set handler and the TUI use one implementation. Include
`gnmi/gnmi.pb.h`. Provide:
- `inline gnmi::Path parse_yang_path(const std::string&)` — port of readline.cpp
  version (strip leading `/`, split on `/`, parse `[k=v]` predicates into
  `elem->mutable_key()`).
- `inline std::string path_to_string(const gnmi::Path&)` — join `/name` +
  `[k=v]` per elem; return `"/"` when empty.
- `inline void set_typed_value(gnmi::TypedValue*, const std::string& v,
   const std::string& enc = "")` — `JSON_IETF`→json_ietf_val; `JSON` or
   looks-like-json (`v.front()` is `{`/`[`)→json_val; else string_val. For the
   `set` command pass enc="" so scalars become string_val, JSON objects json_val.
- `inline std::string typed_value_to_string(const gnmi::TypedValue&)` — switch on
  `value_case()` (string/int/uint/bool/double/json/json_ietf/ascii); default
  `"<value>"`.

### 2. `app/inc/update_sink.hpp` (NEW, header-only singleton)
Decouples the local Set handler from the UI:
```cpp
class update_sink {
public:
  using cb_t = std::function<void(const std::string&)>;
  static update_sink& instance();          // function-local static
  void set(cb_t cb);                        // TUI registers here
  void emit(const std::string& line);       // Set handler calls; no-op if unset
private:
  cb_t m_cb;
};
```
(Header-only inline is fine; only ever one definition used.)

### 3. `app/src/client_app.cpp` (EDIT — Set handler)
Include `update_sink.hpp` and `gnmi_util.hpp`. After building `SetResponse`, for
each update/replace emit
`update_sink::instance().emit("SET <op> " + path_to_string(path) + " = " +
typed_value_to_string(val))`, and for each delete emit the path. No behavior
change when no sink registered (the `app`/`cli` binaries leave it unset).

### 4. `app/peer/inc/endpoint_config.hpp` + `app/peer/src/endpoint_config.cpp` (NEW)
```cpp
struct endpoint { std::string host; uint16_t port{0}; };
struct peer_config { endpoint local; endpoint remote; tls_config tls; };
bool load_peer_config(const std::string& path, peer_config& out, std::string& err);
```
Loader: `lua_file cfg; cfg.process_create_luafile(path);` then read
`cfg.commands().at(path)`; navigate `members["local"]` /
`members["remote"]` (each a `shared_ptr<table_type>`) → `members["endpoint"]`,
which is EITHER a `value_type` string `"host:port"` (split on last `:`) OR a
nested `shared_ptr<table_type>` with `members["ip"]`(string) +
`members["port"]`(int32). Optional `members["tls"]` table →
`{enabled(bool), cert, key, ca}` into `tls_config`. Use `std::get_if` /
`std::holds_alternative`. Return false + err on missing/invalid.

### 5. `app/peer/inc/gnmi_tui.hpp` + `app/peer/src/gnmi_tui.cpp` (NEW)
`class gnmi_tui : public evt_io` constructed with `(STDIN_FILENO, rawfd_tag{})`.
- ncurses init: `initscr(); cbreak(); noecho();` create `m_input_win` (row 0,
  1 line) and `m_output_win` (rows 2..LINES-1, `scrollok(TRUE); idlok(TRUE)`),
  draw divider on row 1. `keypad(m_input_win, TRUE); nodelay(m_input_win, TRUE)`.
  Prompt `"Marvel> "`.
- Override `handle_read(fd,"",false)`: loop `wgetch(m_input_win)` until `ERR`;
  handle Enter(`\n`/`\r`/KEY_ENTER)→dispatch(line)+clear; Backspace(263/127/8)→
  pop_back; Ctrl-D(4)→quit; printable(32..126)→append; redraw input line.
- `println(const std::string&)`: split on `\n`, `wprintw(m_output_win,...)`,
  `wrefresh(m_output_win)`, then `wrefresh(m_input_win)` to keep cursor. Register
  as the `update_sink` callback (prefix rendered lines with `"[remote] "`).
- Command dispatch (accept `gnmi set/get ...` and bare `set/get ...`):
  - `set`: spec = rest of line; split on `,`; each split on FIRST `:` →
    (xpath, value). Build `SetRequest`, `prefix.set_target("ADMIN")`, per pair
    `add_update()` with `parse_yang_path(xpath)` + `set_typed_value(val, value)`.
    Serialize; `gnmi_client::push_async(remote.host, remote.port,
    "/gnmi.gNMI/Set", pb, tls, on_done)`. `on_done` renders SetResponse
    (status/paths/op) or error into output pane. NOTE the `xpath:value` split
    uses the first `:` — document that module-qualified segments (`module:...`)
    are not supported in this shorthand.
  - `get`: spec split on `,`; each is a path; build `GetRequest`,
    `prefix.set_target("VIEWER")`, `add_path(parse_yang_path(p))`,
    `set_encoding(gnmi::JSON)`; push_async to `/gnmi.gNMI/Get`; on_done render
    notifications/updates.
  - `help`, `quit`/`exit`. quit → `event_base_loopbreak(evt_base::instance().get())`.
- Dtor: `endwin()`.
- Holds `endpoint m_remote; tls_config m_tls;` (passed in ctor) for outbound calls.
All ncurses touches happen inside the single event-loop thread (handle_read and
push_async on_done both run there) — safe.

### 6. `app/peer/src/peer_main.cpp` (NEW)
- `--config=` flag (default `/app/command/endpoint.lua`); reuse a small
  `get_flag` like `main_app.cpp`.
- `load_peer_config` → on error print + exit 1.
- Create local `server local_srv(cfg.local.host, cfg.local.port, cfg.tls);`
- Create `gnmi_tui tui(cfg.remote, cfg.tls);` and
  `update_sink::instance().set([&tui](const std::string& s){ tui.println("[remote] " + s); });`
- `run_evt_loop{}();`
- Keep `local_srv` and `tui` in scope for the whole loop.

### 7. `app/peer/CMakeLists.txt` (NEW)
Model on `app/cli/CMakeLists.txt`. Target `gnmi_peer`:
- sources: `peer/src/*.cpp` + shared `../src/framework.cpp`,
  `../src/server_app.cpp`, `../src/client_app.cpp`, `../src/gnmi_client.cpp`,
  `../src/grpc_session.cpp`, `../src/http2.cpp`, `../src/lua_engine.cpp`.
- include dirs: `inc`, `../inc`, `../openvpn/inc` (tls_config.hpp lives under
  openvpn/inc — VERIFY exact path), `${NGHTTP2_INCLUDE_DIRS}`, proto_gen.
- link: `gnmi_proto pthread ssl crypto ncurses ${LIBEVENT_LIBRARIES}
  ${LIBEVENT_OPENSSL_LIBRARIES} ${NGHTTP2_LIBRARIES} ${LUA_LIBRARIES}`.
  (Add `mosquitto` only if a linked source pulls it in — client_app/framework do
  NOT appear to; verify at link time.)
- `find_library(CURSES ...)` or `find_package(Curses REQUIRED)` → link
  `${CURSES_LIBRARIES}`; add `${CURSES_INCLUDE_DIRS}`.

### 8. `app/CMakeLists.txt` (EDIT)
Add `add_subdirectory(peer)` (guarded like the others / unconditional near the
main app target). Ensure it can see `gnmi_proto`.

### 9. `app/command/endpoint.lua` (NEW example)
```lua
return {
  ["local"]  = { endpoint = { ip = "0.0.0.0",   port = 58989 } },
  ["remote"] = { endpoint = { ip = "127.0.0.1", port = 58990 } },
  -- FQDN form alternative:
  --   endpoint = "peer.example.com:58990"
  tls = { enabled = false, cert = "", key = "", ca = "" },
}
```

### 10. `Dockerfile` (EDIT)
- build stage apt: add `libncurses-dev` (next to `libreadline-dev`).
- runtime stage apt: add `libncurses6` (next to `libreadline8`).
- Add `COPY --from=build .../build/app/peer/gnmi_peer /app/gnmi_peer`.
- (endpoint.lua ships via existing `COPY .../app/command/ /app/command/`.)

### 11. Docs (EDIT, optional but recommended)
Add a `gnmi_peer` section to `hackthon/README.md` and/or `docs/services.md`:
config format, both endpoint forms, `gnmi set a:1,b:2` example, two-pane
behavior, how the remote's Set lands in the bottom pane.

## Open items / cautions to resolve during implementation
- Confirm `tls_config.hpp` include path (`app/openvpn/inc/tls_config.hpp`) and
  `build_server_ctx()` / `build_client_ctx()` signatures.
- Verify no `mosquitto` symbol is pulled into the `gnmi_peer` link set; add
  `mosquitto` to link libs only if needed.
- `xpath:value` first-colon split limitation (module-qualified paths) — document.
- ncurses resize (SIGWINCH) not handled in v1 — acceptable for first cut.
- Build cannot be run locally here; validate via
  `docker build -t marvel:release hackthon/` (or `podman build`).

## Suggested implementation order
1. `gnmi_util.hpp` → 2. `update_sink.hpp` → 3. edit `client_app.cpp` →
4. `endpoint_config.*` → 5. `endpoint.lua` → 6. `gnmi_tui.*` →
7. `peer_main.cpp` → 8. `peer/CMakeLists.txt` → 9. edit `app/CMakeLists.txt` →
10. edit `Dockerfile` → 11. docs → 12. docker build to compile-check.
