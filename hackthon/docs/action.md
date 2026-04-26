# How to Request a New evt_io Subclass

Use the template below when you want to add a new networked component to the
codebase.  Answering every field up front removes ambiguity and maps directly
onto the `evt_io` hook methods, so the code can be generated cleanly in one
pass.

---

## Request Template

```
I want to add a <name> that acts as a <client|server>-side component.

1. Connection:  it connects to / accepts from <host:port | unix socket | ...>
                triggered by <CLI command | boot | incoming accept | timer>

2. Protocol:    wire format is <gRPC | raw protobuf | JSON | custom>
                over <TCP | TLS | Unix socket>

3. Data in:     when bytes arrive, decode them as <proto message name>
                and pass to <function / object> for processing

4. Data out:    build a <proto message name>, encode it, send back

5. Lifecycle:
   - on connect  → do <handshake / send greeting / start timer>
   - on data     → do <decode / dispatch / respond>
   - on close    → do <notify parent / retry / set done flag>
   - on timeout  → do <log / reconnect / signal error>

6. Parent:      owned by <server map | standalone | CLI call>
                notifies parent on close? <yes / no>
```

---

## Field → Code Mapping

| Template field | Maps to |
|---|---|
| `name` | Class name inheriting `evt_io` |
| `client\|server` | `evt_io(host, port, outbound)` vs `evt_io(host, port)` constructor |
| Connection trigger | Where the object is instantiated (boot, CLI handler, accept callback) |
| On connect | `handle_connect()` override |
| Data in / out | `handle_read()` override |
| On close | `handle_close()` override |
| On timeout | `handle_event()` override |
| Parent notify | `handle_close()` calls `m_parent->handle_close(channel)` — yes or no |

---

## Example Request

> *"I want a `gnmi_subscriber` that acts as a client-side component.
> It connects to a peer at a configurable host:port, triggered on boot.
> The protocol is gRPC Subscribe over TCP.
> On connect it sends a `SubscribeRequest` proto.
> On each arriving DATA frame it decodes a `SubscribeResponse` proto and
> calls `on_notification(resp)`.
> On close it logs and sets a `done` flag.
> No parent to notify — it is standalone."*

### How that maps

```
name         → gnmi_subscriber : public evt_io
client-side  → evt_io(host, port, /*outbound=*/true)
on connect   → handle_connect(): flush h2 preface, submit SubscribeRequest
on data      → handle_read():    h2.recv() → decode SubscribeResponse → on_notification()
on close     → handle_close():   log, set m_done = true
on timeout   → handle_event():   log, set m_done = true
parent       → none
```

---

## Existing Subclasses for Reference

| Class | Side | Role | Key hook |
|---|---|---|---|
| `connected_client` | server | accepts gNMI RPCs from peer, dispatches via `grpc_session` | `handle_read` feeds `m_grpc->recv()` |
| `gnmi_connection` | client | one-shot unary gRPC call initiated by the CLI | `handle_connect` submits request; `handle_read` feeds `m_h2.recv()` |
| `fs_app` | — | inotify fd watcher, reloads Lua command files | `handle_read` calls `process_inotify_onchange()` |

---

## Virtual Hook Reference (`evt_io`)

```cpp
// Called when an outbound TCP connection is established (BEV_EVENT_CONNECTED).
virtual std::int32_t handle_connect(const std::int32_t &channel,
                                    const std::string &peer_host);

// Called by client_read_cb when bytes arrive.
// dry_run=true: return 0 to confirm you can handle this data.
// dry_run=false: actually process it.
virtual std::int32_t handle_read(const std::int32_t &channel,
                                 const std::string &data,
                                 const bool &dry_run);

// Called by client_event_cb on BEV_EVENT_EOF or BEV_EVENT_ERROR.
virtual std::int32_t handle_close(const std::int32_t &channel);

// Called by client_event_cb on BEV_EVENT_TIMEOUT.
virtual std::int32_t handle_event(const std::int32_t &channel,
                                  const std::uint16_t &event);

// Called by client_write_cb when the send buffer drains.
virtual std::int32_t handle_write(const std::int32_t &channel);

// Called by server_accept_cb when a new inbound connection is accepted.
virtual std::int32_t handle_accept(const std::int32_t &channel,
                                   const std::string &peer_host);
```

---

## Callback → Hook Dispatch (framework.cpp)

```
libevent event
    │
    ▼
client_event_cb(bev, events, ctx)
    │  static_cast<evt_io*>(ctx)   ← safe: all subclasses inherit evt_io
    ├── BEV_EVENT_CONNECTED  → handle_connect()
    ├── BEV_EVENT_EOF/ERROR  → handle_close()
    └── BEV_EVENT_TIMEOUT    → handle_event()

client_read_cb(bev, ctx)
    │  static_cast<evt_io*>(ctx)
    ├── handle_read(..., dry_run=true)   returns 0 → can handle
    └── handle_read(..., dry_run=false)  → process + evbuffer_drain

client_write_cb(bev, ctx)
    └── handle_write()
```

Virtual dispatch ensures the correct derived-class override is called
regardless of the base pointer type held by the callback.
