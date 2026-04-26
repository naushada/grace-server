-- gNMI Set/Replace command.
--
-- Replace completely overwrites the subtree at path with the given value.
-- Any configuration not present in value will be removed.
-- (Equivalent to gnmi SetRequest.replace[])
--
-- Usage at the Tarana> prompt:
--   gnmi_replace target=<ip> port=<port> prefix=<yang-path> path=<yang-path> value=<json> encoding=<JSON|JSON_IETF>
--
-- prefix   - common YANG path prefix (use "/" for root).
-- path     - path of the subtree to replace, relative to prefix.
--            Example: /interface[name=eth0]/config
-- value    - complete replacement value as a JSON object.
--            Example: {"description":"new-uplink","enabled":true,"mtu":9000}
-- encoding - JSON (default) or JSON_IETF.

return {
  gnmi_replace = {
    target   = "127.0.0.1",
    port     = 9339,
    prefix   = "/interfaces",
    path     = "/interface[name=eth0]/config",
    value    = '{"description":"replaced-uplink","enabled":true}',
    encoding = "JSON",
  },
}
