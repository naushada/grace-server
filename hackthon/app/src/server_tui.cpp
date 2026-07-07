#include "server_tui.hpp"
#include "update_sink.hpp"

#include <ncurses.h>
#include <sys/ioctl.h> // ioctl, TIOCGWINSZ, winsize
#include <unistd.h>    // STDIN_FILENO

#include <csignal> // SIGWINCH
#include <sstream>
#include <string>

// ---------------------------------------------------------------------------
// utf8_clip: bytes of `s` that fit within `cols` columns without splitting a
// multibyte sequence (same helper as tunnel_tui).
// ---------------------------------------------------------------------------
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

static short color_id(short fg, bool have_color, short *next) {
  if (!have_color) return 0;
  const short pair = (*next)++;
  init_pair(pair, fg, -1);
  return COLOR_PAIR(pair);
}

// ---------------------------------------------------------------------------
// Construction / teardown
//
// Layout (H rows): row 0 header, rows 1..H-2 transcript, row H-1 footer.
// ---------------------------------------------------------------------------

server_tui::server_tui(std::uint16_t port, const std::string &log_file)
    : evt_io(STDIN_FILENO, rawfd_tag{}), m_port(port), m_log_path(log_file) {
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
  m_attr_head = color_id(COLOR_CYAN, have_color, &next);
  m_attr_leaf = color_id(COLOR_WHITE, have_color, &next);
  m_attr_sync = color_id(COLOR_GREEN, have_color, &next);
  m_attr_warn = color_id(COLOR_YELLOW, have_color, &next);
  m_attr_remote = color_id(COLOR_MAGENTA, have_color, &next);

  // Mouse-wheel scrolling (works inside tmux).
  mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, nullptr);
  mouseinterval(0);

  relayout();

  update_sink::instance().add(
      [this](const std::string &line) { this->println(line); });
  println("Listening. Waiting for gNMI Set pushes / dial-out telemetry …");

  m_winch_ev = evsignal_new(evt_base::instance().get(), SIGWINCH, on_winch, this);
  if (m_winch_ev)
    event_add(m_winch_ev, nullptr);
}

server_tui::~server_tui() {
  update_sink::instance().clear();
  if (m_winch_ev) event_free(m_winch_ev);
  if (m_head) delwin(m_head);
  if (m_out) delwin(m_out);
  if (m_foot) delwin(m_foot);
  curs_set(1);
  endwin();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void server_tui::draw_header() {
  if (!m_head) return;
  int h = 0, w = 0;
  getmaxyx(m_head, h, w);
  (void)h;
  werase(m_head);
  const std::string s = " Marvel gNMI Server · :" + std::to_string(m_port) +
                        " · " + std::to_string(m_lines.size()) +
                        " lines      PgUp/PgDn·End scroll · q quit";
  wattron(m_head, A_DIM);
  mvwaddnstr(m_head, 0, 0, s.c_str(), utf8_clip(s, w > 0 ? w : 0));
  wattroff(m_head, A_DIM);
  wnoutrefresh(m_head);
}

void server_tui::draw_foot() {
  if (!m_foot) return;
  int h = 0, w = 0;
  getmaxyx(m_foot, h, w);
  (void)h;
  werase(m_foot);
  std::string s;
  int attr;
  if (!m_log_path.empty()) {
    s = " ▶ logging to " + m_log_path;
    attr = (m_attr_sync ? m_attr_sync : A_NORMAL) | A_BOLD;
  } else {
    s = " (not logging — start with --log-file=<path> to save updates)";
    attr = A_DIM;
  }
  wattron(m_foot, attr);
  mvwaddnstr(m_foot, 0, 0, s.c_str(), utf8_clip(s, w));
  wattroff(m_foot, attr);
  wnoutrefresh(m_foot);
}

int server_tui::attr_for(const std::string &s) const {
  if (s.rfind("[PushSub]", 0) == 0 || s.rfind("[reg]", 0) == 0 ||
      s.rfind("[tun]", 0) == 0)
    return m_attr_head;
  if (s.find("error") != std::string::npos ||
      s.find("failed") != std::string::npos ||
      s.find("UNIMPLEMENTED") != std::string::npos)
    return m_attr_warn;
  if (s.find("sync") != std::string::npos)
    return m_attr_sync;
  if (s.rfind("[remote]", 0) == 0 || s.rfind("[set]", 0) == 0 ||
      s.rfind("[get]", 0) == 0 || s.rfind("[sub]", 0) == 0)
    return m_attr_remote;
  if (s.rfind("    ", 0) == 0) // indented leaf data
    return m_attr_leaf;
  return 0;
}

void server_tui::push_history(const std::string &part) {
  m_lines.push_back(part);
  if (m_lines.size() > 8000)
    m_lines.pop_front();
}

void server_tui::redraw_out() {
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

void server_tui::println(const std::string &line) {
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
  draw_header();
  redraw_out();
}

void server_tui::scroll_by(int lines) {
  m_scroll += lines;
  if (m_scroll < 0) m_scroll = 0;
  redraw_out();
}

void server_tui::relayout() {
  int H = 0, W = 0;
  getmaxyx(stdscr, H, W);
  if (H < 3 || W < 4) return;

  int out_h = H - 2; // header + footer
  if (out_h < 1) out_h = 1;

  if (m_head) { delwin(m_head); m_head = nullptr; }
  if (m_out) { delwin(m_out); m_out = nullptr; }
  if (m_foot) { delwin(m_foot); m_foot = nullptr; }

  m_head = newwin(1, W, 0, 0);
  m_out = newwin(out_h, W, 1, 0);
  m_foot = newwin(1, W, H - 1, 0);
  scrollok(m_out, FALSE);
  keypad(m_out, TRUE);
  nodelay(m_out, TRUE);

  clear();
  refresh();
  draw_header();
  redraw_out();
  draw_foot();
}

void server_tui::on_winch(int, short, void *arg) {
  auto *self = static_cast<server_tui *>(arg);
  struct winsize ws;
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
      ws.ws_col > 0)
    resize_term(ws.ws_row, ws.ws_col);
  self->relayout();
}

// ---------------------------------------------------------------------------
// Input (libevent EV_READ on stdin)
// ---------------------------------------------------------------------------

std::int32_t server_tui::handle_read(const std::int32_t & /*channel*/,
                                     const std::string & /*data*/,
                                     const bool &dry_run) {
  if (dry_run) return 0;
  int ch = 0;
  while ((ch = wgetch(m_out)) != ERR) {
    if (ch == 'q' || ch == 'Q' || ch == 4) {
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
    } else if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        if (ev.bstate & BUTTON4_PRESSED) scroll_by(3);       // wheel up
        else if (ev.bstate & BUTTON5_PRESSED) scroll_by(-3); // wheel down
      }
    }
  }
  return 0;
}
