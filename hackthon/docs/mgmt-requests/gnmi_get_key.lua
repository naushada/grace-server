-- gNMI Get with an EXPLICIT key map (raw proto form) — equivalent to the
-- "/interfaces/interface[name=eth0]/state" string sugar. A gnmi.Path is built
-- from repeated PathElem; PathElem.key is a map<string,string> written as a
-- nested table of key = value.
return {
  device_id = "",
  request = {
    ["@type"] = "gnmi.GetRequest",
    encoding = "JSON",
    path = {
      {
        elem = {
          { name = "interfaces" },
          { name = "interface", key = { name = "eth0" } }, -- map<string,string>
          { name = "state" },
        },
      },
    },
  },
}
