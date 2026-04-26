-- gNMI Get command.
--
-- Usage at the Tarana> prompt:
--   gnmi_get target=<ip> port=<port> prefix=<yang-path> path=<yang-path> encoding=<JSON|PROTO|JSON_IETF>
--
-- prefix  - common YANG path prefix shared by all paths in this request.
--           Use "/" to indicate the root (no shared prefix).
--           Example: /interfaces/interface[name=eth0]
--
-- path    - specific leaf or subtree path relative to the prefix.
--           Example: state/oper-status
--
-- encoding - wire encoding for returned values: JSON (default), PROTO, JSON_IETF.
--
-- The CLI will build a gnmi.GetRequest proto, gRPC-frame it, and send it to
-- the target device over a plain TCP connection (no TLS).

return {
  gnmi_get = {
    target   = "127.0.0.1",
    port     = 9339,
    prefix   = "/interfaces/interface[name=eth0]",
    path     = "state/oper-status",
    encoding = "JSON",
  },
}
