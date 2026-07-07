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
//   ❯ show interfaces_                              <- command input line
//
// Grpc-tunnel-style, with a bottom input line: type a CLI command + Enter to
// send it (mgmt_hub packs a DeviceRequest with a random rpc_id); DeviceResponse
// replies and unsolicited pushes stream into the transcript. Optional file
// logging is a separate update_sink subscriber; the input line reflects nothing
// about it. Type quit/exit or ^D to leave.

#include "framework.hpp"

#include <cstdint>
#include <deque>
#include <string>

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
  void draw_input();
  void redraw_out();
  void push_history(const std::string &part);
  void scroll_by(int lines);
  void submit_input();
  int attr_for(const std::string &line) const;
  void relayout();
  static void on_winch(int, short, void *arg);
  static void on_tick(int, short, void *arg);

  std::uint16_t m_port;
  std::string m_log_path;
  std::string m_input; // current command being typed
  int m_attr_head{0};
  int m_attr_reply{0};
  int m_attr_push{0};
  int m_attr_leaf{0};
  int m_attr_warn{0};
  struct _win_st *m_head{nullptr};
  struct _win_st *m_sessions{nullptr};
  struct _win_st *m_sep{nullptr};
  struct _win_st *m_out{nullptr};
  struct _win_st *m_inp{nullptr};
  struct event *m_winch_ev{nullptr};
  struct event *m_tick_ev{nullptr};
  std::deque<std::string> m_lines;
  int m_scroll{0};
};

#endif // __mgmt_tui_hpp__
