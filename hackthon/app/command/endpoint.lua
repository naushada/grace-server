-- gnmi_peer endpoint configuration.
--
-- Consumed by the `gnmi_peer` binary (--config=<path>, default
-- /app/command/endpoint.lua). It declares:
--   * local.endpoint  — where THIS process runs its own gNMI server. The
--     remote peer pushes gNMI Set operations here; they appear in the
--     transcript, tagged [remote].
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

  -- Optional TUI palette (interactive mode only). Foreground colours on the
  -- terminal's own background. Each entry accepts a base colour
  -- (black/red/green/yellow/blue/magenta/cyan/white), the aliases
  -- amber(=yellow)/grey, the attributes dim/bold, bright-<colour>, or
  -- default/none. Omit `colors` (or any key) to keep the defaults below.
  colors = {
    remote = "cyan",   -- [remote] pushes (the "diff")
    ok     = "green",  -- [set]/[get] OK
    error  = "amber",  -- errors / usage
    echo   = "dim",    -- echoed commands
  },
}
