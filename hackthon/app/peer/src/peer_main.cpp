// gnmi_peer — a two-pane, config-driven peer-to-peer gNMI shell.
//
//   * Reads local/remote gRPC endpoints from a Lua config (--config).
//   * Runs a local gNMI server at local.endpoint so the remote peer can push
//     Set operations to us (rendered live in the bottom pane).
//   * Sends `gnmi set` / `gnmi get` to remote.endpoint over direct gRPC.
//
// Two front-ends:
//   * ncurses two-pane TUI  — the default on an interactive terminal.
//   * headless line shell    — auto-selected when stdin/stdout is not a TTY
//     (pipe / CI / smoke test). Reads command lines from stdin, prints results
//     and remote pushes to stdout. Force with --headless=true|false.
//
// Usage:
//   gnmi_peer [--config=/app/command/endpoint.lua] [--log=/tmp/gnmi_peer.log]
//             [--headless=true|false]

#include "endpoint_config.hpp"
#include "framework.hpp"
#include "gnmi_cmd.hpp"
#include "gnmi_tui.hpp"
#include "server_app.hpp"
#include "update_sink.hpp"

#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h> // STDIN_FILENO, isatty, read

static std::string get_flag(int argc, const char *argv[],
                            const std::string &name, const std::string &def) {
  const std::string prefix = "--" + name + "=";
  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg.starts_with(prefix))
      return std::string(arg.substr(prefix.size()));
  }
  return def;
}

// ---------------------------------------------------------------------------
// headless_shell — line-based front-end for non-TTY use (pipes / CI).
//
// Wired into the shared libevent loop via the rawfd_tag stdin ctor (like the
// TUI), so the local gNMI server keeps receiving remote pushes while we read
// commands. On stdin EOF: if we have already run at least one command (a
// sender), quit after a short grace period so async responses arrive; if we
// have run none (a pure receiver started with closed stdin), keep serving.
// ---------------------------------------------------------------------------
class headless_shell : public evt_io {
public:
  headless_shell(const endpoint &remote, const tls_config &tls)
      : evt_io(STDIN_FILENO, rawfd_tag{}),
        m_cmd(remote, tls,
              [](const std::string &s) { std::cout << s << "\n"; }) {}

  std::int32_t handle_read(const std::int32_t & /*channel*/,
                           const std::string & /*data*/,
                           const bool &dry_run) override {
    if (dry_run)
      return 0;

    char buf[4096];
    const ssize_t n = ::read(STDIN_FILENO, buf, sizeof(buf));
    if (n == 0) {                      // EOF on stdin
      if (m_dispatched > 0) {          // a sender: let responses land, then quit
        struct timeval tv {
          2, 0
        };
        arm_timer(1, tv, /*repeat=*/false);
      }
      return 0; // a pure receiver keeps serving
    }
    if (n < 0)
      return 0;

    for (ssize_t i = 0; i < n; ++i) {
      const char c = buf[i];
      if (c == '\n')
        flush_line();
      else if (c != '\r')
        m_line.push_back(c);
    }
    return 0;
  }

  std::int32_t handle_timeout(int /*timer_id*/) override {
    event_base_loopbreak(evt_base::instance().get());
    return 0;
  }

private:
  void flush_line() {
    std::string line;
    line.swap(m_line);
    const auto b = line.find_first_not_of(" \t");
    if (b == std::string::npos)
      return; // blank line
    std::cout << "> " << line << "\n";
    ++m_dispatched;
    if (!m_cmd.dispatch(line))
      event_base_loopbreak(evt_base::instance().get());
  }

  gnmi_cmd m_cmd;
  std::string m_line;
  int m_dispatched{0};
};

int main(int argc, const char *argv[]) {
  const std::string config_path =
      get_flag(argc, argv, "config", "/app/command/endpoint.lua");
  const std::string log_path =
      get_flag(argc, argv, "log", "/tmp/gnmi_peer.log");

  // Front-end selection: honour --headless if given, else auto-detect a TTY.
  const std::string headless_flag = get_flag(argc, argv, "headless", "");
  bool headless;
  if (headless_flag == "true")
    headless = true;
  else if (headless_flag == "false")
    headless = false;
  else
    headless = !isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO);

  // Load config first, while std::cerr still reaches the real terminal — so
  // configuration errors are visible before ncurses takes over the screen.
  peer_config cfg;
  std::string err;
  if (!load_peer_config(config_path, cfg, err)) {
    std::cerr << "[gnmi_peer] config error: " << err << "\n";
    return 1;
  }

  if (headless) {
    // Keep logs on stdout (greppable); flush each line for pipes.
    std::cout << std::unitbuf;
    std::cout << "[gnmi_peer] headless config=" << config_path
              << " local=" << cfg.local.host << ":" << cfg.local.port
              << " remote=" << cfg.remote.host << ":" << cfg.remote.port
              << " tls=" << (cfg.tls.enabled ? "ON" : "OFF") << "\n";

    server local_srv(cfg.local.host, cfg.local.port, cfg.tls);
    headless_shell shell(cfg.remote, cfg.tls);
    update_sink::instance().set(
        [](const std::string &line) { std::cout << "[remote] " << line << "\n"; });

    run_evt_loop{}();
    update_sink::instance().clear();
    return 0;
  }

  // Interactive (ncurses) front-end. The linked components log verbosely to
  // std::cout / std::cerr; redirect those C++ streams to a logfile so they
  // don't corrupt the display. ncurses keeps fd 1 (the terminal).
  static std::ofstream log_file(log_path, std::ios::app);
  if (log_file) {
    std::cout.rdbuf(log_file.rdbuf());
    std::cerr.rdbuf(log_file.rdbuf());
    std::freopen(log_path.c_str(), "a", stderr);
  }

  std::cout << "[gnmi_peer] config=" << config_path
            << " local=" << cfg.local.host << ":" << cfg.local.port
            << " remote=" << cfg.remote.host << ":" << cfg.remote.port
            << " tls=" << (cfg.tls.enabled ? "ON" : "OFF") << "\n";

  server local_srv(cfg.local.host, cfg.local.port, cfg.tls);
  gnmi_tui tui(cfg.remote, cfg.tls);
  update_sink::instance().set(
      [&tui](const std::string &line) { tui.println("[remote] " + line); });

  run_evt_loop{}();

  update_sink::instance().clear();
  return 0;
}
