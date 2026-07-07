#ifndef __server_tui_hpp__
#define __server_tui_hpp__

// Live monitor TUI for `app --mode=gnmi-server`.
//
//   Marvel gNMI Server · :58989 · 1240 lines        PgUp/PgDn·End scroll · q quit
//   [PushSub] s3 05:58:52Z /radios/global/state  20u 0d        <- colour-coded
//       /status=1  /sla-profile="max"  /uptime=6251  …            scrolling
//   ── sync ──                                                    transcript
//   ▶ logging to /var/log/updates.jsonl
//
// Renders the incoming update stream (local Set pushes + dial-out
// PushSubscriptionUpdates telemetry) fed through update_sink, colour-coded by
// line type, with scrollback. File logging is done separately (a file sink on
// update_sink); the footer just reflects whether it is active. Read-only:
// PgUp/PgDn/Home/End scroll, q quits.

#include "framework.hpp"

#include <cstdint>
#include <deque>
#include <string>

struct _win_st; // ncurses WINDOW
struct event;   // libevent

class server_tui : public evt_io {
public:
  // log_file is shown in the footer (empty = not logging); the actual writing
  // is a separate update_sink subscriber owned by the caller.
  server_tui(std::uint16_t port, const std::string &log_file);
  ~server_tui() override;

  std::int32_t handle_read(const std::int32_t &channel, const std::string &data,
                           const bool &dry_run) override;

  void println(const std::string &line);

private:
  void draw_header();
  void draw_foot();
  void redraw_out();
  void push_history(const std::string &part);
  void scroll_by(int lines);
  int attr_for(const std::string &line) const;
  void relayout();
  static void on_winch(int, short, void *arg);

  std::uint16_t m_port;
  std::string m_log_path;
  int m_attr_head{0};   // [PushSub]/[reg] header lines
  int m_attr_leaf{0};   // indented leaf data
  int m_attr_sync{0};   // sync markers
  int m_attr_warn{0};   // errors
  int m_attr_remote{0}; // [remote]/[set]/[get]
  struct _win_st *m_head{nullptr};
  struct _win_st *m_out{nullptr};
  struct _win_st *m_foot{nullptr};
  struct event *m_winch_ev{nullptr};
  std::deque<std::string> m_lines;
  int m_scroll{0};
};

#endif // __server_tui_hpp__
