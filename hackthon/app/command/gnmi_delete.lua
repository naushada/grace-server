-- gNMI Set/Delete command.
--
-- Delete removes the subtree at path from the target's configuration.
-- (Equivalent to gnmi SetRequest.delete[])
--
-- Usage at the Tarana> prompt:
--   gnmi_delete target=<ip> port=<port> prefix=<yang-path> path=<yang-path>
--
-- prefix - common YANG path prefix (use "/" for root).
-- path   - path of the node to delete, relative to prefix.
--          Example: /interface[name=eth0]

return {
  gnmi_delete = {
    target = "127.0.0.1",
    port   = 9339,
    prefix = "/interfaces",
    path   = "/interface[name=eth0]",
  },
}
