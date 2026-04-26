-- OpenVPN-like TCP tunnel client configuration.
--
-- Pass this file's parameters on the command line when starting the app in
-- client mode:
--
--   ./app --mode=client \
--         --server=10.0.0.1 \
--         --port=1194 \
--         --tun=tun0 \
--         --status=/run/vpn_status.lua
--
-- Fields:
--   server      - IP address or hostname of the openvpn_server.
--   port        - TCP port the server listens on (default 1194).
--   tun         - TUN interface name to create after IP assignment (Linux only).
--                 The kernel assigns the next free tunX if left as "tun0"
--                 and that name is already in use.
--   status_file - Path where the client writes its connection state as a Lua
--                 table.  Reloaded live by the fs_app inotify watcher.
--                 Format:
--                   return {
--                     vpn_status = {
--                       service_ip = "<assigned virtual IP>",
--                       status     = "Connected" | "Down",
--                       timestamp  = <UTC seconds since epoch>,
--                     },
--                   }
--
-- The client drives the shared libevent event loop (evt_base::instance()).
-- In server mode the same process runs the gNMI server and openvpn_server
-- instead — see openvpn_server.lua for that configuration.

return {
  openvpn_client = {
    server      = "127.0.0.1",
    port        = 1194,
    tun         = "tun0",
    status_file = "/run/vpn_status.lua",
  },
}
