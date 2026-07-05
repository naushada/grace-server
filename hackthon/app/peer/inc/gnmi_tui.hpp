#ifndef __gnmi_tui_hpp__
#define __gnmi_tui_hpp__

// Two-pane ncurses terminal for gnmi_peer.
//
//   +-----------------------------------------------+
//   | Marvel> gnmi set /a/b:5                        |  <- input line (row 0)
//   +-----------------------------------------------+  <- divider (row 1)
//   | [remote] UPDATE /a/b = 5                       |
//   | [set] OK, 1 result(s)                          |  <- scrolling output
//   | ...                                            |
//   +-----------------------------------------------+
//
// Input is driven by the shared libevent loop: gnmi_tui derives from evt_io and
// is constructed on STDIN_FILENO via the rawfd_tag ctor, so handle_read() is
// invoked whenever stdin is readable. Keys are drained non-blocking (nodelay)
// with ncurses wgetch(). The bottom pane is fed both by command results and by
// the update_sink (operations the remote peer pushes into our local server).

#include "endpoint_config.hpp"
#include "framework.hpp"
#include "gnmi_cmd.hpp"

#include <cstdint>
#include <string>

// ncurses WINDOW without pulling <ncurses.h> (and its macros) into this header.
struct _win_st;

class gnmi_tui : public evt_io {
public:
  gnmi_tui(const endpoint &remote, const tls_config &tls,
           const palette_config &colors = {});
  ~gnmi_tui() override;

  // libevent raw-fd read hook — drains queued keystrokes from stdin.
  std::int32_t handle_read(const std::int32_t &channel, const std::string &data,
                           const bool &dry_run) override;

  // Append a (possibly multi-line) message to the scrolling output pane.
  void println(const std::string &line);

private:
  void draw_input();
  void dispatch(const std::string &line);
  int attr_for(const std::string &line) const; // display attr for an output line

  gnmi_cmd m_cmd;
  std::string m_line;
  // Resolved terminal attributes per line category (0 = terminal default).
  int m_attr_remote{0};
  int m_attr_ok{0};
  int m_attr_warn{0};
  int m_attr_echo{0};
  struct _win_st *m_input_win{nullptr};
  struct _win_st *m_output_win{nullptr};
};

#endif // __gnmi_tui_hpp__
