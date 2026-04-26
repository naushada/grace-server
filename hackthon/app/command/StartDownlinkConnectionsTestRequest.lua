-- Sample command definition consumed by lua_file::process_create_luafile.
--
-- The file must `return` a single Lua table whose first-level key is the
-- command name the user will type at the Marvel> prompt. Everything under
-- that key becomes the parameter tree that Tab completion walks.
--
-- Layout assumed by the CLI:
--
--   return {
--     <CommandName> = {
--       <scalar_field>   = <value>,          -- shows up as "<field>="
--       <array_field>    = { v1, v2, ... },  -- shows up as "<field>="
--       <nested_message> = {                 -- shows up as "<field>."
--         <child_field>  = <value>,
--       },
--     },
--   }
--
-- NOTE: fs_app watches "/app/command" at runtime. Copy or symlink this file
-- into that directory so lua_file picks it up on boot (or after inotify
-- delivers IN_CLOSE_WRITE).

return {
  StartDownlinkConnectionsTestRequest = {
    rate_mbps   = 50,
    duration_sec = 30,
    bidirectional = true,
    device_id   = { "device-1", "device-2", "device-3" },

    test_param = {
      mode            = "burst",
      packet_size     = 1500,
      retries         = 3,
      enable_logging  = false,
    },
  },
}
