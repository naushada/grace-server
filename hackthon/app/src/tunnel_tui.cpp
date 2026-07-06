#include "tunnel_tui.hpp"
#include "tunnel_hub.hpp"
#include "update_sink.hpp"

#include <ncurses.h>
#include <sys/ioctl.h> // ioctl, TIOCGWINSZ, winsize
#include <unistd.h>    // STDIN_FILENO

#include <algorithm>
#include <cstdio>
#include <csignal>
#include <ctime>
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

// Bytes of `s` that fit within `cols` columns, counting each UTF-8 code point as
// one column and never splitting a multibyte sequence (clip without wrapping).
static int utf8_clip(const std::string &s, int cols) {
  int bytes = 0, shown = 0;
  const int n = static_cast<int>(s.size());
  while (bytes < n && shown < cols) {
    const unsigned char c = static_cast<unsigned char>(s[bytes]);
    int len = 1;
    if ((c & 0x80) == 0) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    if (bytes + len > n) break;
    bytes += len;
    ++shown;
  }
  return bytes;
}

// Compact uptime: "45s", "3m", "1h2m", "2d3h".
static std::string fmt_uptime(std::time_t secs) {
  if (secs < 0) secs = 0;
  char b[32];
  if (secs < 60)
    std::snprintf(b, sizeof(b), "%llds", static_cast<long long>(secs));
  else if (secs < 3600)
    std::snprintf(b, sizeof(b), "%lldm", static_cast<long long>(secs / 60));
  else if (secs < 86400)
    std::snprintf(b, sizeof(b), "%lldh%lldm", static_cast<long long>(secs / 3600),
                  static_cast<long long>((secs % 3600) / 60));
  else
    std::snprintf(b, sizeof(b), "%lldd%lldh", static_cast<long long>(secs / 86400),
                  static_cast<long long>((secs % 86400) / 3600));
  return b;
}

static short color_id(short fg, bool have_color, short *next) {
  if (!have_color) return 0;
  const short pair = (*next)++;
  init_pair(pair, fg, -1);
  return COLOR_PAIR(pair);
}

// ---------------------------------------------------------------------------
// Construction / teardown
//
// Layout (H rows):
//   row 0              header       (m_head)
//   rows 1..TP         targets pane (m_targets)
//   row  1+TP          separator    (m_sep)
//   rows 2+TP..H-1     transcript   (m_out, scrollback + scrollbar)
// ---------------------------------------------------------------------------

tunnel_tui::tunnel_tui(std::uint16_t port)
    : evt_io(STDIN_FILENO, rawfd_tag{}), m_port(port) {
  initscr();
  cbreak();
  noecho();
  curs_set(0);

  bool have_color = false;
  if (has_colors()) {
    start_color();
    use_default_colors();
    have_color = true;
  }
  short next = 1;
  m_attr_reg = color_id(COLOR_GREEN, have_color, &next);
  m_attr_fwd = color_id(COLOR_CYAN, have_color, &next);
  m_attr_reply = color_id(COLOR_GREEN, have_color, &next);
  m_attr_warn = color_id(COLOR_YELLOW, have_color, &next);

  relayout(); // creates windows + first paint

  // Tunnel events (tunnel_log) arrive via update_sink; render each as a line.
  update_sink::instance().set(
      [this](const std::string &line) { this->println(line); });
  println("Listening. Waiting for tunnel clients to Register targets …");

  // Resize + 1s uptime refresh, both as libevent events.
  m_winch_ev = evsignal_new(evt_base::instance().get(), SIGWINCH, on_winch, this);
  if (m_winch_ev)
    event_add(m_winch_ev, nullptr);
  m_tick_ev = event_new(evt_base::instance().get(), -1, EV_PERSIST, on_tick, this);
  if (m_tick_ev) {
    struct timeval sec{1, 0};
    event_add(m_tick_ev, &sec);
  }
}

tunnel_tui::~tunnel_tui() {
  update_sink::instance().clear();
  if (m_winch_ev) event_free(m_winch_ev);
  if (m_tick_ev) event_free(m_tick_ev);
  if (m_head) delwin(m_head);
  if (m_targets) delwin(m_targets);
  if (m_sep) delwin(m_sep);
  if (m_out) delwin(m_out);
  if (m_foot) delwin(m_foot);
  curs_set(1);
  endwin();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void tunnel_tui::draw_header() {
  if (!m_head) return;
  int h = 0, w = 0;
  getmaxyx(m_head, h, w);
  (void)h;
  werase(m_head);
  const std::string s =
      " Marvel gNMI Tunnel · :" + std::to_string(m_port) + " · " +
      std::to_string(tunnel_hub::instance().size()) +
      " targets      PgUp/PgDn·End scroll · q quit";
  wattron(m_head, A_DIM);
  mvwaddnstr(m_head, 0, 0, s.c_str(), utf8_clip(s, w > 0 ? w : 0));
  wattroff(m_head, A_DIM);
  wnoutrefresh(m_head);
}

void tunnel_tui::draw_targets() {
  if (!m_targets) return;
  int th = 0, w = 0;
  getmaxyx(m_targets, th, w);
  werase(m_targets);

  wattron(m_targets, A_DIM);
  const char *hdr = " TARGET               TYPE          UPTIME";
  mvwaddnstr(m_targets, 0, 0, hdr, utf8_clip(hdr, w));
  wattroff(m_targets, A_DIM);

  auto snap = tunnel_hub::instance().snapshot();
  std::sort(snap.begin(), snap.end(),
            [](const tunnel_hub::target_info &a,
               const tunnel_hub::target_info &b) { return a.target < b.target; });
  const std::time_t now = std::time(nullptr);
  const int rows = th - 1; // rows below the column header
  for (int i = 0; i < static_cast<int>(snap.size()) && i < rows; ++i) {
    if (i == rows - 1 && static_cast<int>(snap.size()) > rows) {
      char more[32];
      std::snprintf(more, sizeof(more), " … +%d more",
                    static_cast<int>(snap.size()) - (rows - 1));
      mvwaddnstr(m_targets, i + 1, 0, more, utf8_clip(more, w));
      break;
    }
    char line[256];
    std::snprintf(line, sizeof(line), " %-20.20s %-12.12s %s",
                  snap[i].target.c_str(), snap[i].type.c_str(),
                  fmt_uptime(now - snap[i].since).c_str());
    std::string ls(line);
    if (m_attr_reg) wattron(m_targets, m_attr_reg);
    mvwaddnstr(m_targets, i + 1, 0, ls.c_str(), utf8_clip(ls, w));
    if (m_attr_reg) wattroff(m_targets, m_attr_reg);
  }
  wnoutrefresh(m_targets);
}

void tunnel_tui::draw_sep() {
  if (!m_sep) return;
  int h = 0, w = 0;
  getmaxyx(m_sep, h, w);
  (void)h;
  werase(m_sep);
  wattron(m_sep, A_DIM);
  mvwhline(m_sep, 0, 0, ACS_HLINE, w);
  wattroff(m_sep, A_DIM);
  wnoutrefresh(m_sep);
}

// Full-width footer with the full target name(s) — the targets pane truncates
// them, so show the complete published name(s) here for visibility.
void tunnel_tui::draw_foot() {
  if (!m_foot) return;
  int h = 0, w = 0;
  getmaxyx(m_foot, h, w);
  (void)h;
  werase(m_foot);
  auto snap = tunnel_hub::instance().snapshot();
  std::string s;
  if (snap.empty()) {
    s = " target: (none registered yet)";
  } else {
    s = " target: ";
    for (std::size_t i = 0; i < snap.size(); ++i) {
      if (i) s += "   ·   ";
      s += snap[i].target;
      if (snap[i].local_port != 0)
        s += "  →  :" + std::to_string(snap[i].local_port);
    }
  }
  const int attr = (m_attr_reg ? m_attr_reg : A_NORMAL) | A_BOLD;
  wattron(m_foot, attr);
  mvwaddnstr(m_foot, 0, 0, s.c_str(), utf8_clip(s, w));
  wattroff(m_foot, attr);
  wnoutrefresh(m_foot);
}

int tunnel_tui::attr_for(const std::string &s) const {
  if (s.find("+target") != std::string::npos) return m_attr_reg;
  if (s.find("-target") != std::string::npos) return m_attr_warn;
  if (s.find("unparsable") != std::string::npos ||
      s.find("error") != std::string::npos) return m_attr_warn;
  if (s.rfind("[reg]", 0) == 0) return m_attr_fwd;
  if (s.rfind("[<-]", 0) == 0) return m_attr_reply;
  return 0;
}

void tunnel_tui::push_history(const std::string &part) {
  m_lines.push_back(part);
  if (m_lines.size() > 5000)
    m_lines.pop_front();
}

void tunnel_tui::redraw_out() {
  if (!m_out) return;
  int H = 0, W = 0;
  getmaxyx(m_out, H, W);
  werase(m_out);

  const int total = static_cast<int>(m_lines.size());
  const int view_h = H;
  int maxscroll = total - view_h;
  if (maxscroll < 0) maxscroll = 0;
  if (m_scroll > maxscroll) m_scroll = maxscroll;
  if (m_scroll < 0) m_scroll = 0;
  int first = total - view_h - m_scroll;
  if (first < 0) first = 0;

  const bool bar = (W > 2 && total > view_h);
  const int text_w = bar ? W - 1 : W;
  for (int row = 0; row < view_h; ++row) {
    const int idx = first + row;
    if (idx < 0 || idx >= total) break;
    const std::string &l = m_lines[idx];
    const int attr = attr_for(l);
    if (attr) wattron(m_out, attr);
    mvwaddnstr(m_out, row, 0, l.c_str(), utf8_clip(l, text_w));
    if (attr) wattroff(m_out, attr);
  }
  if (bar) {
    int thumb = view_h * view_h / total;
    if (thumb < 1) thumb = 1;
    if (thumb > view_h) thumb = view_h;
    const int track = view_h - thumb;
    const int denom = (total - view_h > 0) ? total - view_h : 1;
    int thumb_top = track * first / denom;
    if (thumb_top < 0) thumb_top = 0;
    if (thumb_top > track) thumb_top = track;
    wattron(m_out, A_DIM);
    for (int row = 0; row < view_h; ++row)
      mvwaddstr(m_out, row, W - 1,
                (row >= thumb_top && row < thumb_top + thumb) ? "█" : "░");
    wattroff(m_out, A_DIM);
  }
  wnoutrefresh(m_out);
  doupdate();
}

void tunnel_tui::println(const std::string &line) {
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
  if (m_scroll > 0)
    m_scroll += added; // hold the view when scrolled up
  draw_header();       // target count may have changed
  draw_foot();         // and the target name(s)
  redraw_out();
}

void tunnel_tui::scroll_by(int lines) {
  m_scroll += lines;
  if (m_scroll < 0) m_scroll = 0;
  redraw_out();
}

void tunnel_tui::relayout() {
  int H = 0, W = 0;
  getmaxyx(stdscr, H, W);
  if (H < 5 || W < 4) return; // too small — keep current windows

  int th = H / 3;
  if (th < 4) th = 4;
  if (th > 10) th = 10;
  if (th > H - 4) th = H - 4; // room for header + sep + >=1 transcript + footer
  if (th < 1) th = 1;
  const int out_top = 1 + th + 1;
  int out_h = H - out_top - 1; // -1 for the footer at H-1
  if (out_h < 1) out_h = 1;

  if (m_head) { delwin(m_head); m_head = nullptr; }
  if (m_targets) { delwin(m_targets); m_targets = nullptr; }
  if (m_sep) { delwin(m_sep); m_sep = nullptr; }
  if (m_out) { delwin(m_out); m_out = nullptr; }
  if (m_foot) { delwin(m_foot); m_foot = nullptr; }

  m_head = newwin(1, W, 0, 0);
  m_targets = newwin(th, W, 1, 0);
  m_sep = newwin(1, W, 1 + th, 0);
  m_out = newwin(out_h, W, out_top, 0);
  m_foot = newwin(1, W, H - 1, 0);
  scrollok(m_out, FALSE);
  keypad(m_out, TRUE);
  nodelay(m_out, TRUE);

  clear();
  refresh();
  draw_header();
  draw_targets();
  draw_sep();
  redraw_out();
  draw_foot();
}

void tunnel_tui::on_winch(int, short, void *arg) {
  auto *self = static_cast<tunnel_tui *>(arg);
  struct winsize ws;
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
      ws.ws_col > 0)
    resize_term(ws.ws_row, ws.ws_col);
  self->relayout();
}

void tunnel_tui::on_tick(int, short, void *arg) {
  auto *self = static_cast<tunnel_tui *>(arg);
  self->draw_header();
  self->draw_targets();
  self->draw_foot();
  doupdate();
}

// ---------------------------------------------------------------------------
// Input (libevent EV_READ on stdin)
// ---------------------------------------------------------------------------

std::int32_t tunnel_tui::handle_read(const std::int32_t & /*channel*/,
                                     const std::string & /*data*/,
                                     const bool &dry_run) {
  if (dry_run) return 0;
  int ch = 0;
  while ((ch = wgetch(m_out)) != ERR) {
    if (ch == 'q' || ch == 'Q' || ch == 4) { // q / Ctrl-D → quit
      event_base_loopbreak(evt_base::instance().get());
      return 0;
    } else if (ch == KEY_RESIZE) {
      relayout();
    } else if (ch == KEY_PPAGE) {
      int h = 0, w = 0; getmaxyx(m_out, h, w); (void)w;
      scroll_by(h > 1 ? h - 1 : 1);
    } else if (ch == KEY_NPAGE) {
      int h = 0, w = 0; getmaxyx(m_out, h, w); (void)w;
      scroll_by(-(h > 1 ? h - 1 : 1));
    } else if (ch == KEY_UP) {
      scroll_by(1);
    } else if (ch == KEY_DOWN) {
      scroll_by(-1);
    } else if (ch == KEY_HOME) {
      m_scroll = static_cast<int>(m_lines.size());
      redraw_out();
    } else if (ch == KEY_END) {
      m_scroll = 0;
      redraw_out();
    }
  }
  return 0;
}
