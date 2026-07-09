-- gnmi-cli (gnmi_peer) endpoint config — standalone gNMI client shell.
--
-- Edit `remote` to point at the gNMI target, then run `./service.sh gnmi-cli`.
-- This file is bind-mounted, so every (re)start re-reads it — no rebuild needed.
--
-- `remote` options:
--   - via the tunnel on THIS host (service.sh mgmt / grpc-tunnel up, :9339):
--         ["remote"] = { endpoint = "127.0.0.1:9339" }
--   - directly at a device's gNMI:
--         ["remote"] = { endpoint = "<device-ip>:<port>" }
--
-- `local` is the client's own gNMI server (for peer-to-peer / receiving pushes).
-- Keep it off :58989 if mgmt/tunnel are running on this host (they use :58989).
--
-- If the target's gNMI is TLS, set tls.enabled=true and give the ca (that TLS is
-- end-to-end to the device, independent of any tunnel control channel).
return {
  ["local"]  = { endpoint = { ip = "0.0.0.0", port = 58990 } },
  ["remote"] = { endpoint = "127.0.0.1:9339" },
  tls = { enabled = false, cert = "", key = "", ca = "" },
}
