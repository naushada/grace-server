-- Peer A: local gNMI server on 58989; sends to Peer B (resolved by the compose
-- service name "peerB"). "host:port" string endpoint form.
return {
  ["local"]  = { endpoint = { ip = "0.0.0.0", port = 58989 } },
  ["remote"] = { endpoint = "peerB:58990" },
  tls = { enabled = false, cert = "", key = "", ca = "" },
}
