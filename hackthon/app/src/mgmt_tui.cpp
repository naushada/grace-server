#include "mgmt_tui.hpp"
#include "mgmt_hub.hpp"
#include "update_sink.hpp"

#include <ncurses.h>
#include <sys/ioctl.h> // ioctl, TIOCGWINSZ, winsize
#include <unistd.h>    // STDIN_FILENO

#include <csignal> // SIGWINCH
#include <ctime>
#include <sstream>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// helpers (same as tunnel_tui — internal linkage, no ODR clash)
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

static std::string fmt_uptime(std::time_t secs) {
  if (secs < 0) secs = 0;
  char b[32];
  if (secs < 60)
    std::snprintf(b, sizeof(b), "%llds", static_cast<long long>(secs));
  else if (secs < 3600)
    std::snprintf(b, sizeof(b), "%lldm", static_cast<long long>(secs / 60));
  else
    std::snprintf(b, sizeof(b), "%lldh%lldm", static_cast<long long>(secs / 3600),
                  static_cast<long long>((secs % 3600) / 60));
  return b;
}

static std::string trim(const std::string &s) {
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string::npos) return "";
  const auto e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
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
// Layout (H rows): row 0 header, rows 1..TP sessions, row 1+TP separator,
// rows 2+TP..H-2 transcript, row H-1 command input line.
// ---------------------------------------------------------------------------

mgmt_tui::mgmt_tui(std::uint16_t port, const std::string &log_file)
    : evt_io(STDIN_FILENO, rawfd_tag{}), m_port(port), m_log_path(log_file) {
  initscr();
  cbreak();
  noecho();
  curs_set(1); // input line — show the caret

  bool have_color = false;
  if (has_colors()) {
    start_color();
    use_default_colors();
    have_color = true;
  }
  short next = 1;
  m_attr_head = color_id(COLOR_CYAN, have_color, &next);
  m_attr_reply = color_id(COLOR_GREEN, have_color, &next);
  m_attr_push = color_id(COLOR_MAGENTA, have_color, &next);
  m_attr_leaf = color_id(COLOR_WHITE, have_color, &next);
  m_attr_warn = color_id(COLOR_YELLOW, have_color, &next);

  relayout();

  update_sink::instance().add(
      [this](const std::string &line) { this->println(line); });
  println("Ready. Waiting for a device to open DialTcc.Subscribe …");
  println("Type a CLI command + Enter to send it; quit/exit or ^D to leave.");

  m_winch_ev = evsignal_new(evt_base::instance().get(), SIGWINCH, on_winch, this);
  if (m_winch_ev) event_add(m_winch_ev, nullptr);
  m_tick_ev = event_new(evt_base::instance().get(), -1, EV_PERSIST, on_tick, this);
  if (m_tick_ev) {
    struct timeval sec{1, 0};
    event_add(m_tick_ev, &sec);
  }
}

mgmt_tui::~mgmt_tui() {
  update_sink::instance().clear();
  if (m_winch_ev) event_free(m_winch_ev);
  if (m_tick_ev) event_free(m_tick_ev);
  if (m_head) delwin(m_head);
  if (m_sessions) delwin(m_sessions);
  if (m_sep) delwin(m_sep);
  if (m_out) delwin(m_out);
  if (m_inp) delwin(m_inp);
  curs_set(1);
  endwin();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void mgmt_tui::draw_header() {
  if (!m_head) return;
  int h = 0, w = 0;
  getmaxyx(m_head, h, w);
  (void)h;
  werase(m_head);
  std::string s = " Marvel gNMI Mgmt · :" + std::to_string(m_port) + " · " +
                  std::to_string(mgmt_hub::instance().size()) + " session(s)";
  if (!m_log_path.empty()) s += " · log " + m_log_path;
  s += "      PgUp/PgDn·End scroll · ^D quit";
  wattron(m_head, A_DIM);
  mvwaddnstr(m_head, 0, 0, s.c_str(), utf8_clip(s, w > 0 ? w : 0));
  wattroff(m_head, A_DIM);
  wnoutrefresh(m_head);
}

void mgmt_tui::draw_sessions() {
  if (!m_sessions) return;
  int th = 0, w = 0;
  getmaxyx(m_sessions, th, w);
  werase(m_sessions);
  wattron(m_sessions, A_DIM);
  const char *hdr = " SESSION   UPTIME";
  mvwaddnstr(m_sessions, 0, 0, hdr, utf8_clip(hdr, w));
  wattroff(m_sessions, A_DIM);

  auto snap = mgmt_hub::instance().snapshot();
  const std::time_t now = std::time(nullptr);
  const int rows = th - 1;
  for (int i = 0; i < static_cast<int>(snap.size()) && i < rows; ++i) {
    char line[128];
    std::snprintf(line, sizeof(line), " #%-7d %s", snap[i].id,
                  fmt_uptime(now - snap[i].since).c_str());
    std::string ls(line);
    if (m_attr_reply) wattron(m_sessions, m_attr_reply);
    mvwaddnstr(m_sessions, i + 1, 0, ls.c_str(), utf8_clip(ls, w));
    if (m_attr_reply) wattroff(m_sessions, m_attr_reply);
  }
  wnoutrefresh(m_sessions);
}

void mgmt_tui::draw_sep() {
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

void mgmt_tui::draw_input() {
  if (!m_inp) return;
  int h = 0, w = 0;
  getmaxyx(m_inp, h, w);
  (void)h;
  werase(m_inp);
  const std::string prompt = " \xE2\x9D\xAF "; // " ❯ "
  wattron(m_inp, A_BOLD);
  mvwaddnstr(m_inp, 0, 0, prompt.c_str(), utf8_clip(prompt, w));
  wattroff(m_inp, A_BOLD);
  const int px = 3; // prompt columns
  if (w > px)
    mvwaddnstr(m_inp, 0, px, m_input.c_str(), utf8_clip(m_input, w - px));
  wmove(m_inp, 0, px + static_cast<int>(m_input.size())); // caret at end
  wnoutrefresh(m_inp);
}

int mgmt_tui::attr_for(const std::string &s) const {
  if (s.find("unparsable") != std::string::npos ||
      s.find("[stderr]") != std::string::npos ||
      s.find("not sent") != std::string::npos)
    return m_attr_warn;
  if (s.rfind("[mgmt] reply", 0) == 0) return m_attr_reply;
  if (s.rfind("[mgmt] push", 0) == 0) return m_attr_push;
  if (s.rfind("[mgmt]", 0) == 0) return m_attr_head;
  if (s.rfind("    ", 0) == 0) return m_attr_leaf;
  return 0;
}

void mgmt_tui::push_history(const std::string &part) {
  m_lines.push_back(part);
  if (m_lines.size() > 8000) m_lines.pop_front();
}

void mgmt_tui::redraw_out() {
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
}

void mgmt_tui::println(const std::string &line) {
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
  if (m_scroll > 0) m_scroll += added; // hold view when scrolled up
  draw_header();
  redraw_out();
  draw_input(); // keep the caret on the input line
  doupdate();
}

void mgmt_tui::scroll_by(int lines) {
  m_scroll += lines;
  if (m_scroll < 0) m_scroll = 0;
  redraw_out();
  draw_input();
  doupdate();
}

void mgmt_tui::submit_input() {
  const std::string line = trim(m_input);
  m_input.clear();
  draw_input();
  if (line.empty()) { doupdate(); return; }
  if (line == "quit" || line == "exit") {
    event_base_loopbreak(evt_base::instance().get());
    return;
  }
  // Split into command + args (whitespace-separated).
  std::istringstream ss(line);
  std::string cmd;
  ss >> cmd;
  std::vector<std::string> args;
  std::string a;
  while (ss >> a) args.push_back(a);

  const std::string rpc_id = mgmt_hub::instance().send_cli(cmd, args, "");
  if (rpc_id.empty())
    println("[mgmt] no session connected — not sent: '" + line + "'");
  else
    println("[mgmt] \xE2\x86\x92 '" + line + "'  rpc=" + rpc_id); // →
}

void mgmt_tui::relayout() {
  int H = 0, W = 0;
  getmaxyx(stdscr, H, W);
  if (H < 6 || W < 4) return;

  int th = H / 4;
  if (th < 3) th = 3;
  if (th > 8) th = 8;
  if (th > H - 5) th = H - 5;
  if (th < 1) th = 1;
  const int out_top = 1 + th + 1;
  int out_h = H - out_top - 1; // -1 for the input line at H-1
  if (out_h < 1) out_h = 1;

  if (m_head) { delwin(m_head); m_head = nullptr; }
  if (m_sessions) { delwin(m_sessions); m_sessions = nullptr; }
  if (m_sep) { delwin(m_sep); m_sep = nullptr; }
  if (m_out) { delwin(m_out); m_out = nullptr; }
  if (m_inp) { delwin(m_inp); m_inp = nullptr; }

  m_head = newwin(1, W, 0, 0);
  m_sessions = newwin(th, W, 1, 0);
  m_sep = newwin(1, W, 1 + th, 0);
  m_out = newwin(out_h, W, out_top, 0);
  m_inp = newwin(1, W, H - 1, 0);
  scrollok(m_out, FALSE);
  keypad(m_inp, TRUE);
  nodelay(m_inp, TRUE);

  clear();
  refresh();
  draw_header();
  draw_sessions();
  draw_sep();
  redraw_out();
  draw_input();
  doupdate();
}

void mgmt_tui::on_winch(int, short, void *arg) {
  auto *self = static_cast<mgmt_tui *>(arg);
  struct winsize ws;
  if (ioctl(STDIN_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
      ws.ws_col > 0)
    resize_term(ws.ws_row, ws.ws_col);
  self->relayout();
}

void mgmt_tui::on_tick(int, short, void *arg) {
  auto *self = static_cast<mgmt_tui *>(arg);
  self->draw_header();
  self->draw_sessions();
  self->draw_input();
  doupdate();
}

// ---------------------------------------------------------------------------
// Input (libevent EV_READ on stdin) — command line + scroll keys
// ---------------------------------------------------------------------------

std::int32_t mgmt_tui::handle_read(const std::int32_t & /*channel*/,
                                   const std::string & /*data*/,
                                   const bool &dry_run) {
  if (dry_run) return 0;
  int ch = 0;
  while ((ch = wgetch(m_inp)) != ERR) {
    if (ch == 4) { // Ctrl-D → quit
      event_base_loopbreak(evt_base::instance().get());
      return 0;
    } else if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
      submit_input();
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
      if (!m_input.empty()) m_input.pop_back();
      draw_input();
      doupdate();
    } else if (ch == KEY_RESIZE) {
      relayout();
    } else if (ch == KEY_PPAGE) {
      int h = 0, w = 0; getmaxyx(m_out, h, w); (void)w;
      scroll_by(h > 1 ? h - 1 : 1);
    } else if (ch == KEY_NPAGE) {
      int h = 0, w = 0; getmaxyx(m_out, h, w); (void)w;
      scroll_by(-(h > 1 ? h - 1 : 1));
    } else if (ch == KEY_HOME) {
      m_scroll = static_cast<int>(m_lines.size());
      redraw_out(); draw_input(); doupdate();
    } else if (ch == KEY_END) {
      m_scroll = 0;
      redraw_out(); draw_input(); doupdate();
    } else if (ch >= 32 && ch < 127) { // printable → append to the command
      m_input.push_back(static_cast<char>(ch));
      draw_input();
      doupdate();
    }
  }
  return 0;
}
