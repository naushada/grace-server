-- gNMI Set/Update command.
--
-- Update merges the supplied value into the existing configuration at path.
-- (Equivalent to gnmi SetRequest.update[])
--
-- All update operations MUST carry role=ADMIN — the server rejects Set
-- requests from VIEWER with gRPC status 7 (PERMISSION_DENIED).
--
-- Updates are routed via the OpenVPN TCP tunnel before reaching the gNMI
-- target.  The CLI connects to tunnel_host:tunnel_port first, receives a
-- virtual IP from the pool, then proceeds with the gNMI Set.  Leave
-- tunnel_host empty to skip the tunnel (dev/testing only).
--
-- Usage at the Tarana> prompt:
--   gnmi_update target=<ip> port=<port> prefix=<yang-path> path=<yang-path> value=<json>
--               encoding=<JSON|JSON_IETF> role=ADMIN tunnel_host=<ip> tunnel_port=<port>

return {
  gnmi_update = {
    target      = "127.0.0.1",
    port        = 9339,
    prefix      = "/interfaces/interface[name=eth0]",
    path        = "config/description",
    value       = "uplink-to-spine",
    encoding    = "JSON",
    role        = "ADMIN",
    tunnel_host = "127.0.0.1",
    tunnel_port = 1194,
  },
}
