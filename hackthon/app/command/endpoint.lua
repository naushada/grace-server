-- gnmi_peer endpoint configuration.
--
-- Consumed by the `gnmi_peer` binary (--config=<path>, default
-- /app/command/endpoint.lua). It declares:
--   * local.endpoint  — where THIS process runs its own gNMI server. The
--     remote peer pushes gNMI Set operations here; they appear in the bottom
--     pane of the two-window terminal.
--   * remote.endpoint — where `gnmi set` / `gnmi get` are sent (direct gRPC
--     over HTTP/2).
--
-- `local` and `remote` are Lua keywords, so they must be written as the quoted
-- table keys ["local"] / ["remote"].
--
-- Each endpoint may be given in either form:
--   endpoint = { ip = "0.0.0.0", port = 58989 }   -- ip + port table
--   endpoint = "peer.example.com:58990"           -- "host:port" (FQDN) string

return {
  ["local"]  = { endpoint = { ip = "0.0.0.0",   port = 58989 } },
  ["remote"] = { endpoint = { ip = "127.0.0.1", port = 58990 } },

  -- Optional TLS for both the local server and outgoing calls. Leave
  -- enabled=false for plain TCP (dev/testing).
  tls = {
    enabled = false,
    cert    = "",
    key     = "",
    ca      = "",
  },
}
