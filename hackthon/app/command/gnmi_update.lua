-- gNMI Set/Update command.
--
-- Update merges the supplied value into the existing configuration at path.
-- (Equivalent to gnmi SetRequest.update[])
--
-- Usage at the Tarana> prompt:
--   gnmi_update target=<ip> port=<port> prefix=<yang-path> path=<yang-path> value=<json> encoding=<JSON|JSON_IETF>
--
-- prefix   - common YANG path prefix (use "/" for root).
-- path     - path of the leaf / subtree to update, relative to prefix.
--            Example: config/description
-- value    - new value as a JSON string or a plain scalar string.
--            Example: "uplink-to-spine"   (plain string, no outer quotes at CLI)
--            Example: {"mtu":9000}        (JSON object)
-- encoding - JSON (default) or JSON_IETF.

return {
  gnmi_update = {
    target   = "127.0.0.1",
    port     = 9339,
    prefix   = "/interfaces/interface[name=eth0]",
    path     = "config/description",
    value    = "uplink-to-spine",
    encoding = "JSON",
  },
}
