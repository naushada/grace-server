-- gNMI Set. Each Update has a `path` (string sugar; keys via [k=v]) and a `val`
-- (a gnmi.TypedValue): set exactly ONE of its oneof fields —
--   string_val | int_val | uint_val | bool_val | double_val |
--   json_val | json_ietf_val | ascii_val   (json_* take a JSON string).
-- `replace` has the same shape as `update`; `delete` is a list of paths.
return {
  device_id = "",                                   -- empty => the BN
  request = {
    ["@type"] = "gnmi.SetRequest",

    update = {
      { path = "/system/config/hostname",
        val  = { string_val = "bn-1" } },

      { path = "/interfaces/interface[name=eth0]/config/enabled",
        val  = { bool_val = true } },

      { path = "/interfaces/interface[name=eth0]/config/mtu",
        val  = { uint_val = 9000 } },

      -- a whole subtree as a JSON value
      { path = "/system/config",
        val  = { json_val = '{"hostname":"bn-1","domain":"lab"}' } },
    },

    -- replace = {
    --   { path = "/system/config/motd", val = { string_val = "hello" } },
    -- },

    -- delete = { "/system/config/motd" },          -- repeated Path (string sugar)
  },
}
