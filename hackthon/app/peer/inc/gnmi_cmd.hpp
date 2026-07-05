#ifndef __gnmi_cmd_hpp__
#define __gnmi_cmd_hpp__

// Core gNMI command handling shared by the ncurses TUI (gnmi_tui) and the
// headless line shell (peer_main). Parses "gnmi set/get ..." command lines,
// builds the proto, sends it to the remote endpoint via
// gnmi_client::push_async, and renders results/errors through a caller-supplied
// output callback (so the TUI can route to its pane and the headless shell to
// stdout).

#include "endpoint_config.hpp"
#include "gnmi_client.hpp"

#include <functional>
#include <string>

class gnmi_cmd {
public:
  using out_fn = std::function<void(const std::string &)>;

  gnmi_cmd(endpoint remote, tls_config tls, out_fn out);

  // Handle one command line. Returns false when the command is quit/exit so the
  // caller can stop its event loop; true otherwise.
  bool dispatch(const std::string &line);

private:
  void do_set(const std::string &spec);
  void do_get(const std::string &spec);
  void render_set_resp(const gnmi_client::response &r);
  void render_get_resp(const gnmi_client::response &r);
  void help();

  endpoint m_remote;
  tls_config m_tls;
  out_fn m_out;
};

#endif // __gnmi_cmd_hpp__
