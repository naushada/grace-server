#ifndef __tunnel_tui_hpp__
#define __tunnel_tui_hpp__

// Live monitor TUI for `app --mode=grpc-tunnel-server`.
//
//   Marvel gNMI Tunnel · :58989 · 2 targets      PgUp/PgDn·End scroll · q quit
//   TARGET              UPTIME     REQS
//   dev1                3m         12
//   dev2                1m          3
//   ──────────────────────────────────────────────────────────────────────
//   [reg] dev1  (1 connected)                    <- scrolling event
//   [->] dev1  /gnmi.gNMI/Get  id=7                 transcript (scrollback)
//   [<-] reply id=7  (128 bytes)
//
// Input is driven by the shared libevent loop (stdin); a 1s timer refreshes the
// targets pane so uptimes tick. Read-only: PgUp/PgDn/Home/End scroll, q quits.

#include "framework.hpp"

#include <cstdint>
#include <deque>
#include <string>

struct _win_st; // ncurses WINDOW
struct event;   // libevent

class tunnel_tui : public evt_io {
public:
  explicit tunnel_tui(std::uint16_t port);
  ~tunnel_tui() override;

  std::int32_t handle_read(const std::int32_t &channel, const std::string &data,
                           const bool &dry_run) override;

  // Append one event line to the scrolling transcript.
  void println(const std::string &line);

private:
  void draw_header();
  void draw_targets();
  void draw_sep();
  void redraw_out(); // transcript viewport + scrollbar
  void push_history(const std::string &part);
  void scroll_by(int lines);
  int attr_for(const std::string &line) const;
  void relayout();
  static void on_winch(int, short, void *arg);
  static void on_tick(int, short, void *arg);

  std::uint16_t m_port;
  int m_attr_reg{0};
  int m_attr_fwd{0};
  int m_attr_reply{0};
  int m_attr_warn{0};
  struct _win_st *m_head{nullptr};    // header row
  struct _win_st *m_targets{nullptr}; // targets pane
  struct _win_st *m_sep{nullptr};     // separator row
  struct _win_st *m_out{nullptr};     // event transcript
  struct event *m_winch_ev{nullptr};  // SIGWINCH (resize)
  struct event *m_tick_ev{nullptr};   // 1s uptime refresh
  std::deque<std::string> m_lines;    // transcript scrollback buffer
  int m_scroll{0};                    // viewport offset from bottom
  int m_targets_h{6};                 // targets pane height (incl. column head)
};

#endif // __tunnel_tui_hpp__
