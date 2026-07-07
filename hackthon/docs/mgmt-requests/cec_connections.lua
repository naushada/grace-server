-- cec_cli command over mgmt dial-out: cec_cli=true makes the device prefix
-- "cec_cli"; json=true appends "--json". This runs `cec_cli connections_show
-- --json` on the BN. Interactive equivalent: `:set cec on` then
-- `connections_show`.
return {
  device_id = "",
  request = {
    ["@type"] = "tnmi.DeviceRequest.CliRequest",
    cmd = "connections_show",
    cec_cli = true,
    json = true,
  },
}
