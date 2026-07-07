-- gNMI Get. Paths use the string sugar (a string assigned to a gnmi.Path field
-- is parsed as a YANG path); keys are the [k=v] predicates in the path string.
return {
  device_id = "RN-147",
  request = {
    ["@type"] = "gnmi.GetRequest",
    encoding = "JSON",                                  -- enum by name
    path = {
      "/system/state",
      "/interfaces/interface[name=eth0]/state",         -- key via [name=eth0]
    },
  },
}
