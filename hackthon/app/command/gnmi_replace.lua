-- gNMI Set/Replace command.
--
-- Replace completely overwrites the subtree at path with the given value.
-- Any configuration not present in value will be removed.
-- (Equivalent to gnmi SetRequest.replace[])
--
-- All replace operations MUST carry role=ADMIN — the server rejects Set
-- requests from VIEWER with gRPC status 7 (PERMISSION_DENIED).
--
-- Updates are routed via the OpenVPN TCP tunnel before reaching the gNMI
-- target.  Leave tunnel_host empty to skip the tunnel (dev/testing only).
--
-- Usage at the Tarana> prompt:
--   gnmi_replace target=<ip> port=<port> prefix=<yang-path> path=<yang-path> value=<json>
--                encoding=<JSON|JSON_IETF> role=ADMIN tunnel_host=<ip> tunnel_port=<port>

return {
  gnmi_replace = {
    target      = "127.0.0.1",
    port        = 9339,
    prefix      = "/interfaces",
    path        = "/interface[name=eth0]/config",
    value       = '{"description":"replaced-uplink","enabled":true}',
    encoding    = "JSON",
    role        = "ADMIN",
    tunnel_host = "127.0.0.1",
    tunnel_port = 1194,
  },
}
