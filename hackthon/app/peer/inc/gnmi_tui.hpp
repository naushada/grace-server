// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#ifndef __gnmi_tui_hpp__
#define __gnmi_tui_hpp__

// Claude-style ncurses terminal for gnmi_peer.
//
//   Marvel gNMI · local :58989 → 127.0.0.1:58990        <- dim header (row 0)
//
//   [remote] UPDATE /a/b = 5                             <- scrolling
//   [set] OK, 1 result(s)                                   transcript
//   ...
//
//   ╭────────────────────────────────────────────╮       <- input box
//   │ ❯ gnmi set /a/b:5                           │          (3 rows,
//   ╰────────────────────────────────────────────╯           bottom)
//     set · get · help · quit                            <- dim hint (last row)
//
// Input is driven by the shared libevent loop: gnmi_tui derives from evt_io and
// is constructed on STDIN_FILENO via the rawfd_tag ctor, so handle_read() is
// invoked whenever stdin is readable. Keys are drained non-blocking (nodelay)
// with ncurses wgetch(). The transcript is fed both by command results and by
// the update_sink (operations the remote peer pushes into our local server).

#include "endpoint_config.hpp"
#include "framework.hpp"
#include "gnmi_cmd.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <vector>

// ncurses WINDOW without pulling <ncurses.h> (and its macros) into this header.
struct _win_st;
// libevent event (for the SIGWINCH handler) without pulling in event2 here.
struct event;

class gnmi_tui : public evt_io {
public:
  gnmi_tui(const endpoint &local, const endpoint &remote, const tls_config &tls,
           const palette_config &colors = {});
  ~gnmi_tui() override;

  // libevent raw-fd read hook — drains queued keystrokes from stdin.
  std::int32_t handle_read(const std::int32_t &channel, const std::string &data,
                           const bool &dry_run) override;

  // Append a (possibly multi-line) message to the scrolling transcript.
  void println(const std::string &line);

private:
  void draw_chrome();                           // header + hint (drawn once)
  void draw_box();                              // input box + cursor
  void dispatch(const std::string &line);
  int attr_for(const std::string &line) const;  // display attr for an output line
  void redraw_out();                            // render the transcript viewport + scrollbar
  void push_history(const std::string &part);   // buffer a transcript line (capped)
  void scroll_by(int lines);                    // move the viewport (+ up / - down); 0-clamps
  void relayout();                              // rebuild windows on terminal resize
  static void on_winch(int, short, void *arg);  // libevent SIGWINCH callback

  gnmi_cmd m_cmd;
  std::string m_line;
  std::string m_header; // dim header text (endpoints)
  // Resolved terminal attributes per line category (0 = terminal default).
  int m_attr_remote{0};
  int m_attr_ok{0};
  int m_attr_warn{0};
  int m_attr_echo{0};
  struct _win_st *m_head{nullptr}; // header row
  struct _win_st *m_out{nullptr};  // scrolling transcript
  struct _win_st *m_box{nullptr};  // bordered input box (3 rows)
  struct _win_st *m_hint{nullptr}; // hint row
  struct event *m_winch_ev{nullptr};    // SIGWINCH (terminal resize) watcher
  std::deque<std::string> m_lines;      // transcript history (scrollback buffer)
  int m_scroll{0};                      // viewport offset from bottom (0 = follow newest)
  int m_hscroll{0};                     // horizontal scroll offset (columns), for wide output
  std::vector<std::string> m_history;   // entered commands (Up/Down recall)
  int m_hist_idx{0};                    // cursor into m_history (== size => new line)
};

#endif // __gnmi_tui_hpp__
