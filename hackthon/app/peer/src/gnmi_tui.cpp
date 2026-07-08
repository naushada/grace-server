// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#include "gnmi_tui.hpp"

#include <ncurses.h>
#include <sys/ioctl.h> // ioctl, TIOCGWINSZ, struct winsize
#include <unistd.h>    // STDIN_FILENO

#include <csignal> // SIGWINCH
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::string trim(const std::string &s) {
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string::npos)
    return "";
  const auto e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

// Total display columns of a UTF-8 string (each sequence = 1 column here).
static int utf8_cols(const std::string &s) {
  int bytes = 0, cols = 0;
  const int n = static_cast<int>(s.size());
  while (bytes < n) {
    const unsigned char c = static_cast<unsigned char>(s[bytes]);
    int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
              : (c & 0xF8) == 0xF0 ? 4 : 1;
    bytes += (bytes + len <= n) ? len : 1;
    ++cols;
  }
  return cols;
}

// UTF-8 substring covering display columns [start_col, start_col+max_cols),
// never splitting a multibyte sequence (for horizontal scrolling).
static std::string utf8_window(const std::string &s, int start_col, int max_cols) {
  const int n = static_cast<int>(s.size());
  auto clen = [&](int b) {
    const unsigned char c = static_cast<unsigned char>(s[b]);
    return (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
           : (c & 0xF8) == 0xF0 ? 4 : 1;
  };
  int bytes = 0, col = 0;
  while (bytes < n && col < start_col) { bytes += clen(bytes); ++col; }
  const int b0 = bytes;
  int shown = 0;
  while (bytes < n && shown < max_cols) {
    const int len = clen(bytes);
    if (bytes + len > n) break;
    bytes += len;
    ++shown;
  }
  return s.substr(b0, bytes - b0);
}

// Map a base colour name to its ncurses COLOR_* id, or -1 if unknown.
static short color_id(const std::string &n) {
  if (n == "black") return COLOR_BLACK;
  if (n == "red") return COLOR_RED;
  if (n == "green") return COLOR_GREEN;
  if (n == "yellow" || n == "amber") return COLOR_YELLOW;
  if (n == "blue") return COLOR_BLUE;
  if (n == "magenta" || n == "purple") return COLOR_MAGENTA;
  if (n == "cyan") return COLOR_CYAN;
  if (n == "white") return COLOR_WHITE;
  if (n == "grey" || n == "gray") return COLOR_WHITE; // dimmed below
  return -1;
}

// Resolve a palette name to a terminal attribute, allocating a colour pair
// (foreground on the default background) from *next_pair when a colour is used.
//   dim / bold           -> attribute only, no colour
//   default / none / ""  -> terminal default (0)
//   bright-<colour>      -> colour + A_BOLD
//   grey / gray          -> white + A_DIM
//   <base colour>        -> that colour
// Falls back to 0 for unknown names, or when the terminal has no colour.
static int resolve_attr(std::string name, bool have_color, short *next_pair) {
  if (name == "dim") return A_DIM;
  if (name == "bold") return A_BOLD;
  if (name.empty() || name == "default" || name == "none") return 0;

  int extra = 0;
  if (name.rfind("bright-", 0) == 0) {
    extra = A_BOLD;
    name = name.substr(7);
  }
  if (name == "grey" || name == "gray") extra |= A_DIM;

  if (!have_color) return extra; // colour unavailable: keep any dim/bold
  const short fg = color_id(name);
  if (fg < 0) return extra; // unknown colour name
  const short pair = (*next_pair)++;
  init_pair(pair, fg, -1);
  return COLOR_PAIR(pair) | extra;
}

// ---------------------------------------------------------------------------
// Construction / teardown
//
// Layout (H rows, W cols):
//   row 0            header      (m_head)
//   rows 1..H-5      transcript  (m_out, scrolling)
//   rows H-4..H-2    input box   (m_box, 3 rows, rounded border)
//   row H-1          hint        (m_hint)
// ---------------------------------------------------------------------------

gnmi_tui::gnmi_tui(const endpoint &local, const endpoint &remote,
                   const tls_config &tls, const palette_config &colors)
    : evt_io(STDIN_FILENO, rawfd_tag{}),
      m_cmd(remote, tls,
            [this](const std::string &line) { println(line); }) {
  m_header = " Marvel gNMI · local :" + std::to_string(local.port) + " → " +
             remote.host + ":" + std::to_string(remote.port) +
             (tls.enabled ? "  (TLS)" : "");

  initscr();
  cbreak();
  noecho();
  curs_set(1);

  // Foreground colours on the terminal's own background (bg = -1), so lines are
  // tinted without any background fill. Palette names come from the Lua config.
  bool have_color = false;
  if (has_colors()) {
    start_color();
    use_default_colors();
    have_color = true;
  }
  short next_pair = 1;
  m_attr_remote = resolve_attr(colors.remote, have_color, &next_pair);
  m_attr_ok = resolve_attr(colors.ok, have_color, &next_pair);
  m_attr_warn = resolve_attr(colors.error, have_color, &next_pair);
  m_attr_echo = resolve_attr(colors.echo, have_color, &next_pair);
  // Distinct colours per gNMI verb result (get / set / subscribe).
  m_attr_get = resolve_attr("cyan", have_color, &next_pair);
  m_attr_set = resolve_attr("green", have_color, &next_pair);
  m_attr_sub = resolve_attr("magenta", have_color, &next_pair);

  int H = 0, W = 0;
  getmaxyx(stdscr, H, W);
  int out_h = H - 5; // header(1) + box(3) + hint(1)
  if (out_h < 1)
    out_h = 1;

  m_head = newwin(1, W, 0, 0);
  m_out = newwin(out_h, W, 1, 0);
  m_box = newwin(3, W, H - 4, 0);
  m_hint = newwin(1, W, H - 1, 0);

  scrollok(m_out, FALSE); // full-window viewport render; no auto-scroll
  keypad(m_box, TRUE);
  nodelay(m_box, TRUE);

  refresh(); // paint the (blank) stdscr once so the sub-windows show through
  draw_chrome();

  println("Ready. Local server is up; commands go to " + remote.host + ":" +
          std::to_string(remote.port) + ".");
  draw_box();

  // Watch for terminal resizes. Input is libevent-driven on stdin, and a
  // SIGWINCH is not stdin data, so without this the resize would only be
  // noticed on the next keystroke. Handle it as a libevent signal event.
  m_winch_ev = evsignal_new(evt_base::instance().get(), SIGWINCH, on_winch,
                            this);
  if (m_winch_ev)
    event_add(m_winch_ev, nullptr);
}

gnmi_tui::~gnmi_tui() {
  if (m_winch_ev) event_free(m_winch_ev);
  if (m_head) delwin(m_head);
  if (m_out) delwin(m_out);
  if (m_box) delwin(m_box);
  if (m_hint) delwin(m_hint);
  curs_set(1);
  endwin();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void gnmi_tui::draw_chrome() {
  werase(m_head);
  wattron(m_head, A_DIM);
  mvwaddstr(m_head, 0, 1, m_header.c_str());
  wattroff(m_head, A_DIM);
  wnoutrefresh(m_head);

  werase(m_hint);
  wattron(m_hint, A_DIM);
  mvwaddstr(
      m_hint, 0, 3,
      "set · get · help · quit  ·  ↑↓ history · PgUp/PgDn·Shift+↑↓·←→ scroll");
  wattroff(m_hint, A_DIM);
  wnoutrefresh(m_hint);
  doupdate();
}

void gnmi_tui::draw_box() {
  int bh = 0, bw = 0;
  getmaxyx(m_box, bh, bw);
  (void)bh;
  werase(m_box);

  // Rounded border, softly dimmed.
  wattron(m_box, A_DIM);
  mvwaddstr(m_box, 0, 0, "╭");
  mvwaddstr(m_box, 0, bw - 1, "╮");
  mvwaddstr(m_box, 2, 0, "╰");
  mvwaddstr(m_box, 2, bw - 1, "╯");
  for (int x = 1; x < bw - 1; ++x) {
    mvwaddstr(m_box, 0, x, "─");
    mvwaddstr(m_box, 2, x, "─");
  }
  mvwaddstr(m_box, 1, 0, "│");
  mvwaddstr(m_box, 1, bw - 1, "│");
  wattroff(m_box, A_DIM);

  // Prompt + the visible tail of the current line.
  const int text_col = 2;    // where "❯ " starts
  const int prompt_cols = 2; // "❯ " occupies two columns
  int avail = bw - text_col - prompt_cols - 2; // keep a right margin
  std::string shown = m_line;
  if (avail < 0)
    avail = 0;
  if (static_cast<int>(shown.size()) > avail)
    shown = shown.substr(shown.size() - avail);

  if (m_attr_ok) wattron(m_box, m_attr_ok);
  mvwaddstr(m_box, 1, text_col, "❯ ");
  if (m_attr_ok) wattroff(m_box, m_attr_ok);
  mvwaddstr(m_box, 1, text_col + prompt_cols, shown.c_str());

  int cursx = text_col + prompt_cols + static_cast<int>(shown.size());
  if (cursx > bw - 2)
    cursx = bw - 2;
  // Visible block cursor — under tmux/MobaXterm the real cursor often doesn't
  // blink, so draw a reverse-video cell where the next character will land.
  wattron(m_box, A_REVERSE);
  mvwaddch(m_box, 1, cursx, ' ');
  wattroff(m_box, A_REVERSE);
  wmove(m_box, 1, cursx);
  wrefresh(m_box);
}

// Classify an output line by its leading tag and return its resolved attribute.
int gnmi_tui::attr_for(const std::string &s) const {
  auto starts = [&](const char *p) { return s.rfind(p, 0) == 0; };
  if (starts("❯"))
    return m_attr_echo;
  if (starts("[remote]"))
    return m_attr_remote;
  if (starts("[notif ") || starts("[sub]")) // subscribe lifecycle + notifications
    return m_attr_sub;
  if (starts("[set]"))
    return m_attr_set;
  if (starts("[get]"))
    return m_attr_get;
  // gnmi get leaf lines are bare (indented) JSON objects → tint like the get result.
  {
    const std::size_t i = s.find_first_not_of(' ');
    if (i != std::string::npos && s[i] == '{')
      return m_attr_get;
  }
  if (s.find("error") != std::string::npos ||
      s.find("denied") != std::string::npos ||
      s.find("FAIL") != std::string::npos || starts("unknown command") ||
      starts("usage") || starts("no valid") || starts("  skip"))
    return m_attr_warn;
  return 0; // terminal default
}

// Buffer a transcript line for the scrollback view. Capped so a long-running
// session (constant telemetry) does not grow without bound.
void gnmi_tui::push_history(const std::string &part) {
  m_lines.push_back(part);
  if (m_lines.size() > 5000)
    m_lines.pop_front();
}

// Render the visible slice of the scrollback buffer into m_out, with a
// scrollbar in the last column. m_scroll is the offset from the bottom
// (0 = pinned to the newest line).
void gnmi_tui::redraw_out() {
  if (!m_out)
    return;
  int H = 0, W = 0;
  getmaxyx(m_out, H, W);
  werase(m_out);

  const int total = static_cast<int>(m_lines.size());
  const int view_h = H - 1; // bottom row is the horizontal scrollbar

  int maxscroll = total - view_h;
  if (maxscroll < 0)
    maxscroll = 0;
  if (m_scroll > maxscroll)
    m_scroll = maxscroll;
  if (m_scroll < 0)
    m_scroll = 0;

  int first = total - view_h - m_scroll; // index of the top visible line
  if (first < 0)
    first = 0;

  const bool vbar = (W > 2 && total > view_h);
  const int text_w = vbar ? W - 1 : W; // reserve the last column for the v-scrollbar

  // Widest visible line → bound horizontal scroll + size the h-scrollbar.
  int maxw = 0;
  for (int row = 0; row < view_h; ++row) {
    const int idx = first + row;
    if (idx < 0 || idx >= total)
      break;
    const int lw = utf8_cols(m_lines[idx]);
    if (lw > maxw)
      maxw = lw;
  }
  int maxh = maxw - text_w;
  if (maxh < 0)
    maxh = 0;
  if (m_hscroll > maxh)
    m_hscroll = maxh;
  if (m_hscroll < 0)
    m_hscroll = 0;

  for (int row = 0; row < view_h; ++row) {
    const int idx = first + row;
    if (idx < 0 || idx >= total)
      break;
    const std::string &l = m_lines[idx];
    const int attr = attr_for(l);
    if (attr)
      wattron(m_out, attr);
    const std::string win = utf8_window(l, m_hscroll, text_w);
    mvwaddnstr(m_out, row, 0, win.c_str(), static_cast<int>(win.size()));
    if (attr)
      wattroff(m_out, attr);
  }

  if (vbar) {
    int thumb = view_h * view_h / total; // thumb height ∝ visible fraction
    if (thumb < 1)
      thumb = 1;
    if (thumb > view_h)
      thumb = view_h;
    const int track = view_h - thumb;
    const int denom = (total - view_h > 0) ? total - view_h : 1;
    int thumb_top = track * first / denom;
    if (thumb_top < 0)
      thumb_top = 0;
    if (thumb_top > track)
      thumb_top = track;
    wattron(m_out, A_DIM);
    for (int row = 0; row < view_h; ++row) {
      const bool on = (row >= thumb_top && row < thumb_top + thumb);
      mvwaddstr(m_out, row, W - 1, on ? "█" : "░");
    }
    wattroff(m_out, A_DIM);
  }

  // Horizontal scrollbar on the bottom row (thumb fills the track if it fits).
  {
    int thumb = maxw > 0 ? text_w * text_w / maxw : text_w;
    if (thumb < 1)
      thumb = 1;
    if (thumb > text_w)
      thumb = text_w;
    const int track = text_w - thumb;
    int thumb_left = maxh > 0 ? track * m_hscroll / maxh : 0;
    if (thumb_left < 0)
      thumb_left = 0;
    if (thumb_left > track)
      thumb_left = track;
    wattron(m_out, A_DIM);
    for (int x = 0; x < text_w; ++x)
      mvwaddstr(m_out, view_h, x,
                (x >= thumb_left && x < thumb_left + thumb) ? "\xE2\x96\x84"   // ▄
                                                            : "\xE2\x94\x80"); // ─
    wattroff(m_out, A_DIM);
  }

  wnoutrefresh(m_out);
  doupdate();
}

// Move the viewport by `lines` (positive = scroll up into history, negative =
// toward newest). Clamped in redraw_out().
void gnmi_tui::scroll_by(int lines) {
  m_scroll += lines;
  if (m_scroll < 0)
    m_scroll = 0;
  redraw_out();
}

void gnmi_tui::println(const std::string &line) {
  std::istringstream ss(line);
  std::string part;
  int added = 0;
  while (std::getline(ss, part, '\n')) {
    push_history(part);
    ++added;
  }
  if (added == 0) {
    push_history("");
    added = 1;
  }
  // If the user has scrolled up, keep their view anchored to the same lines as
  // new output arrives; otherwise stay pinned to the newest line.
  if (m_scroll > 0)
    m_scroll += added;
  redraw_out();
  draw_box(); // keep the cursor in the input box
}

// Rebuild the window layout for the current terminal size (SIGWINCH). Windows
// are recreated (simpler and always correct versus wresize/mvwin ordering) and
// the buffered transcript is replayed so scrollback survives the resize.
void gnmi_tui::relayout() {
  int H = 0, W = 0;
  getmaxyx(stdscr, H, W);
  if (H < 5 || W < 4)
    return; // too small to lay out; keep the current windows

  const int out_h = (H - 5 < 1) ? 1 : H - 5;

  if (m_head) { delwin(m_head); m_head = nullptr; }
  if (m_out)  { delwin(m_out);  m_out = nullptr; }
  if (m_box)  { delwin(m_box);  m_box = nullptr; }
  if (m_hint) { delwin(m_hint); m_hint = nullptr; }

  m_head = newwin(1, W, 0, 0);
  m_out = newwin(out_h, W, 1, 0);
  m_box = newwin(3, W, H - 4, 0);
  m_hint = newwin(1, W, H - 1, 0);

  scrollok(m_out, FALSE);
  keypad(m_box, TRUE);
  nodelay(m_box, TRUE);

  clear();
  refresh();
  draw_chrome();
  redraw_out();
  draw_box();
}

// libevent SIGWINCH callback: sync ncurses to the new terminal size, then
// re-lay-out. Doing it here (rather than relying on ncurses' own KEY_RESIZE)
// means resizes are handled immediately, not just on the next keystroke.
void gnmi_tui::on_winch(int, short, void *arg) {
  auto *self = static_cast<gnmi_tui *>(arg);
  struct winsize ws;
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
      ws.ws_col > 0)
    resize_term(ws.ws_row, ws.ws_col);
  self->relayout();
}

// ---------------------------------------------------------------------------
// Input handling (driven by libevent EV_READ on stdin)
// ---------------------------------------------------------------------------

std::int32_t gnmi_tui::handle_read(const std::int32_t & /*channel*/,
                                   const std::string & /*data*/,
                                   const bool &dry_run) {
  if (dry_run)
    return 0;

  int ch = 0;
  while ((ch = wgetch(m_box)) != ERR) {
    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
      const std::string cmd = m_line;
      m_line.clear();
      draw_box();
      const std::string t = trim(cmd);
      if (!t.empty()) {
        if (m_history.empty() || m_history.back() != t)
          m_history.push_back(t);
        m_hist_idx = static_cast<int>(m_history.size());
        dispatch(cmd);
      }
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
      if (!m_line.empty())
        m_line.pop_back();
      draw_box();
    } else if (ch == 4) { // Ctrl-D → quit
      event_base_loopbreak(evt_base::instance().get());
      return 0;
    } else if (ch == KEY_RESIZE) { // resize delivered via input path
      relayout();
    } else if (ch == KEY_PPAGE) { // Page Up — scroll into history
      int h = 0, w = 0;
      getmaxyx(m_out, h, w);
      (void)w;
      scroll_by(h > 1 ? h - 1 : 1);
    } else if (ch == KEY_NPAGE) { // Page Down — toward newest
      int h = 0, w = 0;
      getmaxyx(m_out, h, w);
      (void)w;
      scroll_by(-(h > 1 ? h - 1 : 1));
    } else if (ch == KEY_SR) { // Shift+Up — scroll output up one line
      scroll_by(1);
    } else if (ch == KEY_SF) { // Shift+Down — scroll output down one line
      scroll_by(-1);
    } else if (ch == KEY_UP) { // recall previous command
      if (!m_history.empty() && m_hist_idx > 0) {
        --m_hist_idx;
        m_line = m_history[m_hist_idx];
        draw_box();
      }
    } else if (ch == KEY_DOWN) { // recall next command (past newest clears)
      if (m_hist_idx < static_cast<int>(m_history.size())) {
        ++m_hist_idx;
        m_line = (m_hist_idx < static_cast<int>(m_history.size()))
                     ? m_history[m_hist_idx]
                     : std::string();
        draw_box();
      }
    } else if (ch == KEY_LEFT) { // pan left (wide output)
      m_hscroll -= 8;
      if (m_hscroll < 0)
        m_hscroll = 0;
      redraw_out();
      draw_box();
    } else if (ch == KEY_RIGHT) { // pan right (clamped to content width)
      m_hscroll += 8;
      redraw_out();
      draw_box();
    } else if (ch == KEY_HOME) { // jump to the oldest buffered line
      m_scroll = static_cast<int>(m_lines.size());
      redraw_out();
    } else if (ch == KEY_END) { // jump to newest and resume follow
      m_scroll = 0;
      redraw_out();
    } else if (ch >= 32 && ch < 127) {
      m_line.push_back(static_cast<char>(ch));
      draw_box();
    }
    // Other keys (arrows, function keys) are ignored in this first cut.
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Command dispatch — echo into the transcript, then delegate to the core.
// ---------------------------------------------------------------------------

void gnmi_tui::dispatch(const std::string &line) {
  println("❯ " + trim(line));
  if (!m_cmd.dispatch(line))
    event_base_loopbreak(evt_base::instance().get());
}
