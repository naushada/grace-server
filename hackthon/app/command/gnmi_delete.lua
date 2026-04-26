-- gNMI Set/Delete command.
--
-- Delete removes the subtree at path from the target's configuration.
-- (Equivalent to gnmi SetRequest.delete[])
--
-- All delete operations MUST carry role=ADMIN — the server rejects Set
-- requests from VIEWER with gRPC status 7 (PERMISSION_DENIED).
--
-- Updates are routed via the OpenVPN TCP tunnel before reaching the gNMI
-- target.  Leave tunnel_host empty to skip the tunnel (dev/testing only).
--
-- Usage at the Tarana> prompt:
--   gnmi_delete target=<ip> port=<port> prefix=<yang-path> path=<yang-path>
--               role=ADMIN tunnel_host=<ip> tunnel_port=<port>

return {
  gnmi_delete = {
    target      = "127.0.0.1",
    port        = 9339,
    prefix      = "/interfaces",
    path        = "/interface[name=eth0]",
    role        = "ADMIN",
    tunnel_host = "127.0.0.1",
    tunnel_port = 1194,
  },
}
