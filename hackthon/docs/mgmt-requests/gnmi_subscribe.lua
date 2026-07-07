-- gNMI Subscribe (STREAM, SAMPLE every 10s). Enums (STREAM/SAMPLE/JSON) are
-- given by name; sample_interval is nanoseconds; paths use the string sugar.
return {
  device_id = "",
  request = {
    ["@type"] = "gnmi.SubscribeRequest",
    subscribe = {
      mode = "STREAM",
      encoding = "JSON",
      subscription = {
        { path = "/interfaces", mode = "SAMPLE", sample_interval = 10000000000 },
      },
    },
  },
}
