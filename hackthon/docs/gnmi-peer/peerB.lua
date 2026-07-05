-- Peer B: local gNMI server on 58990; sends to Peer A (resolved by the compose
-- service name "peerA"). { ip=, port= } table endpoint form.
return {
  ["local"]  = { endpoint = { ip = "0.0.0.0", port = 58990 } },
  ["remote"] = { endpoint = { ip = "peerA", port = 58989 } },
  tls = { enabled = false, cert = "", key = "", ca = "" },
}
