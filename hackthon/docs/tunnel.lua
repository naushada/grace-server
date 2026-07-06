-- grpc-tunnel-server config — app --mode=grpc-tunnel-server --config=<this file>
--
-- One server can front several devices: each dial-out device Registers a target
-- name; you map a local gNMI port to each target here. The port is YOUR choice
-- (the admin's) — the device never asks for a port, it only publishes a target.
--
-- listeners is a map of "port" (string key) -> published target name. (The Lua
-- loader can't represent an array of tables, so the port is a string key.)

return {
  -- Control/tunnel port the devices dial into.
  port = 58989,

  -- TLS for the control channel (device ↔ server). mTLS when `ca` is set.
  --   enabled=false -> plaintext h2c
  --   one-way TLS   -> enabled=true, cert+key (no ca)
  --   mutual TLS    -> enabled=true, cert+key+ca (ca = the device client-cert CA)
  tls = { enabled = false, cert = "", key = "", ca = "" },

  -- Data-plane listeners: local port -> the target it fronts. Point a gNMI
  -- client at 127.0.0.1:<port> (or the host IP) and it reaches that device.
  listeners = {
    ["9339"] = "<serial>|<model>|grpc-tunnel|<sw-version>", -- the device's published target
    -- ["9340"] = "<another-published-target>",
    -- ["9341"] = "<yet-another>",
  },
}
