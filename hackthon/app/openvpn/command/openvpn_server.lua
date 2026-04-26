-- OpenVPN-like TCP tunnel server configuration.
--
-- Usage at the Marvel> prompt:
--   openvpn_server listen_host=<ip> listen_port=<port> pool_network=<net> pool_start=<n> pool_end=<n>
--
-- listen_host  - IP address the tunnel server binds to (default "0.0.0.0" = all interfaces).
-- listen_port  - TCP port to listen on (default 1194, the standard OpenVPN port).
-- pool_network - First three octets of the virtual /24 (default "10.8.0").
--                Clients are assigned addresses from pool_network.pool_start to
--                pool_network.pool_end.
-- pool_start   - First assignable host octet (default 2, i.e. 10.8.0.2).
--                10.8.0.1 is reserved for the server itself.
-- pool_end     - Last assignable host octet (default 254).
--                Effective capacity = pool_end - pool_start + 1 clients.
-- max_clients  - Informational cap shown in logs; hard limit is pool capacity.
--
-- The server uses the libevent framework already present in the application —
-- openvpn_server inherits evt_io (server-side constructor) and creates one
-- openvpn_peer per accepted TCP connection.  Each peer receives an IP_ASSIGN
-- frame immediately on connect.
--
-- Frame format (shared between server and openvpn_tunnel_client):
--   [type:1][length:4 BE][payload:N]
--     0x01  IP_ASSIGN   server→client  assigned virtual IP as ASCII string
--     0x02  DATA        bidirectional  raw tunnel bytes
--     0x03  DISCONNECT  client→server  graceful close

return {
  openvpn_server = {
    listen_host  = "0.0.0.0",
    listen_port  = 1194,
    pool_network = "10.8.0",
    pool_start   = 2,
    pool_end     = 254,
    max_clients  = 64,
  },
}
