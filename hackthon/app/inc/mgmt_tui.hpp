#ifndef __mgmt_tui_hpp__
#define __mgmt_tui_hpp__

// Interactive monitor + command console for `app --mode=mgmt-dialout`.
//
//   Marvel gNMI Mgmt · :58989 · 1 session        PgUp/PgDn·End scroll · ^D quit
//   SESSION   UPTIME
//   #1        2m
//   ──────────────────────────────────────────────────────────────────────
//   [mgmt] → 'show version'  rpc=r-8f3a…            <- colour-coded transcript
//   [mgmt] reply 'show version'  rpc=r-8f3a…           (results + proactive
//       exit=0  12ms                                     pushes)
//   {"sw":"SYS.A3…"}
//   BN(bn-1)> show interfaces_                      <- input line; the prompt is
//                                                      role(hostname) from an
//                                                      on-open /system/state probe
//
// Grpc-tunnel-style, with a bottom input line: type a command + Enter to send
// it (mgmt_hub packs a DeviceRequest with a random rpc_id); replies + proactive
// pushes stream into the transcript. Input model:
//   <cmd> [args...]            run a CLI command on the BN
//   gnmi get <xpath>[,…]       gNMI Get (packed into DeviceRequest.request)
//   gnmi set <xpath>:<val>[,…] gNMI Set
//   gnmi subscribe <xpath> [interval]   gNMI Subscribe (SAMPLE if interval given)
//   send <file.lua>            build a whole DeviceRequest from a Lua file
//   @<device_id> <cmd> ...     run on a specific device (inline, per command)
//   :set cec on|off            sticky CliRequest.cec_cli
//   :set json on|off           sticky CliRequest.json
//   :set timeout <dur>         sticky CliRequest.timeout (10s/500ms/1m…)
//   :show / :reset             list / clear sticky settings (shown in header)
//   quit / exit / ^D           leave
// Up/Down recall command history; a footer above the input line shows each
// session and the device_id it's talking to. Optional file logging is a
// separate update_sink subscriber.

#include "framework.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

struct _win_st; // ncurses WINDOW
struct event;   // libevent

class mgmt_tui : public evt_io {
public:
  mgmt_tui(std::uint16_t port, const std::string &log_file);
  ~mgmt_tui() override;

  std::int32_t handle_read(const std::int32_t &channel, const std::string &data,
                           const bool &dry_run) override;

  void println(const std::string &line);

private:
  void draw_header();
  void draw_sessions();
  void draw_sep();
  void draw_foot(); // session → device summary (above the input line)
  void draw_input();
  void redraw_out();
  void push_history(const std::string &part);
  void scroll_by(int lines);
  void submit_input();
  void send_gnmi(const std::string &verb, const std::string &spec,
                 const std::string &device_id);
  void send_file(const std::string &path); // build a DeviceRequest from a .lua
  int attr_for(const std::string &line) const;
  void relayout();
  static void on_winch(int, short, void *arg);
  static void on_tick(int, short, void *arg);

  std::uint16_t m_port;
  std::string m_log_path;
  std::string m_input;               // current command being typed
  std::vector<std::string> m_history; // submitted commands (Up/Down recall)
  int m_hist_idx{0};                  // cursor into m_history (== size => new)
  // Sticky CliRequest settings (device_id is inline, per command, via @<id>).
  bool m_cec{false};
  bool m_json{false};
  std::uint64_t m_timeout_ns{0};
  std::string m_timeout_disp;
  int m_attr_head{0};
  int m_attr_reply{0};
  int m_attr_push{0};
  int m_attr_leaf{0};
  int m_attr_warn{0};
  struct _win_st *m_head{nullptr};
  struct _win_st *m_sessions{nullptr};
  struct _win_st *m_sep{nullptr};
  struct _win_st *m_out{nullptr};
  struct _win_st *m_foot{nullptr};
  struct _win_st *m_inp{nullptr};
  struct event *m_winch_ev{nullptr};
  struct event *m_tick_ev{nullptr};
  std::deque<std::string> m_lines;
  int m_scroll{0};
};

#endif // __mgmt_tui_hpp__
