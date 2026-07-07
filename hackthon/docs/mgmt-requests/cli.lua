-- A DeviceRequest carrying a CLI command, sent with `send <this file>` in the
-- mgmt TUI. rpc_id is stamped by the sender. The whole table IS the
-- tnmi.DeviceRequest; the inner op is chosen by @type and packed into the Any.
return {
  device_id = "",                          -- empty => the BN
  request = {
    ["@type"] = "tnmi.DeviceRequest.CliRequest",
    cmd = "show",
    args = { "version" },
    cec_cli = true,
    json = true,
  },
}
