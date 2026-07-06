-- gnmi_peer pointed at the tunnel's local gNMI listener.
--
-- Type `gnmi get` / `gnmi set` / `gnmi subscribe` in the peer and it is
-- byte-proxied through the grpc-tunnel-server to the NATed device — you never
-- touch the tunnel internals. `remote` resolves the compose service name
-- "tunnel" and its :9339 data-plane listener.
--
-- If the device's LOCAL gNMI is TLS, set tls.enabled=true here (+ ca) — that TLS
-- is end-to-end to the device, independent of the tunnel's control channel.
return {
  ["local"]  = { endpoint = { ip = "0.0.0.0", port = 58989 } }, -- peer's own server (unused here)
  ["remote"] = { endpoint = "tunnel:9339" },                    -- the tunnel listener
  tls = { enabled = false, cert = "", key = "", ca = "" },
}
