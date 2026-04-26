-- OpenVPN-like TCP tunnel client configuration.
--
-- Pass these parameters on the command line when starting the app in
-- client mode:
--
--   ./app --mode=client \
--         --server=10.0.0.1 \
--         --port=1194 \
--         --status=/run/vpn_status.lua
--
-- Fields:
--   server      - IP address or hostname of the openvpn_server.
--   port        - TCP port the server listens on (default 1194).
--   status_file - Path where the client writes its connection state as a Lua
--                 table.  Reloaded live by the fs_app inotify watcher.
--
-- TUN interface: the client does NOT specify an interface name.  It passes
-- an empty ifr_name to the TUNSETIFF ioctl; the kernel allocates the next
-- free tunX (e.g. tun0, tun1 …) and returns the actual name.  That name is
-- recorded in the status_file under the tun_if field.
--
-- Status file format (written on connect and on disconnect):
--   return {
--     vpn_status = {
--       service_ip = "<server-assigned virtual IP>",
--       tun_if     = "<kernel-assigned interface, e.g. tun1>",
--       status     = "Connected" | "Down",
--       timestamp  = <UTC seconds since epoch>,
--     },
--   }

return {
  openvpn_client = {
    server      = "127.0.0.1",
    port        = 1194,
    status_file = "/run/vpn_status.lua",
  },
}
