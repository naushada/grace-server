#include "help.hpp"

const char *const kCliBanner =
    "Marvel gNMI CLI - ready (type 'help' for commands)";

void print_help(std::ostream &os,
                const std::map<std::string, lua_file::table_type> &lua_commands) {
  os << "Built-in:\n"
        "  clients                       list connected OpenVPN client VIPs\n"
        "  help                          show this message\n"
        "gNMI (routed to target=<vip> over MQTT-via-VPN):\n"
        "  gnmi_get      target=<vip> path=<yang> [prefix=/] [encoding=JSON]\n"
        "  gnmi_update   target=<vip> path=<yang> value=<v> [role=ADMIN]\n"
        "  gnmi_replace  target=<vip> path=<yang> value=<v> [role=ADMIN]\n"
        "  gnmi_delete   target=<vip> path=<yang>          [role=ADMIN]\n";
  if (!lua_commands.empty()) {
    os << "Lua commands loaded from /app/command:\n";
    for (const auto &[file_name, top_table] : lua_commands) {
      (void)file_name;
      for (const auto &[k, _] : top_table.members) {
        (void)_;
        os << "  " << k << '\n';
      }
    }
  }
}
