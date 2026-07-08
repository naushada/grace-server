// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#include "mgmt_tui.hpp"
#include "gnmi_util.hpp"
#include "lua_engine.hpp"
#include "lua_proto.hpp"
#include "mgmt_hub.hpp"
#include "update_sink.hpp"

#include "gnmi/gnmi.pb.h"
#include "dialout/tnmi_dialout.pb.h"

#include <ncurses.h>
#include <sys/ioctl.h> // ioctl, TIOCGWINSZ, winsize
#include <unistd.h>    // STDIN_FILENO

#include <cctype>  // isdigit
#include <csignal> // SIGWINCH
#include <cstdint>
#include <cstdlib> // strtod
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

// Column width of `s` (each UTF-8 code point counts as one column).
static int utf8_cols(const std::string &s) {
  int cols = 0;
  for (std::size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int len = 1;
    if ((c & 0x80) == 0) len = 1;
    else if ((c & 0xE0) == 0xC0) len = 2;
    else if ((c & 0xF0) == 0xE0) len = 3;
    else if ((c & 0xF8) == 0xF0) len = 4;
    i += len;
    ++cols;
  }
  return cols;
}

// Printable column width, ignoring ANSI escape sequences (for h-scroll bounds).
static int visible_cols(const std::string &s) {
  int cols = 0;
  const std::size_t n = s.size();
  for (std::size_t i = 0; i < n;) {
    if (s[i] == '\x1b') {
      if (i + 1 < n && s[i + 1] == '[') {
        i += 2;
        while (i < n && !std::isalpha((unsigned char)s[i])) ++i;
        if (i < n) ++i;
      } else if (i + 1 < n && s[i + 1] == ']') {
        i += 2;
        while (i < n && s[i] != '\x07') ++i;
        if (i < n) ++i;
      } else {
        ++i;
      }
      continue;
    }
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
              : (c & 0xF8) == 0xF0 ? 4 : 1;
    i += (i + len <= n) ? len : 1;
    ++cols;
  }
  return cols;
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

// "10s"/"500ms"/"250us"/"1m"/"2h"/bare-number(seconds) -> nanoseconds; 0 on error.
static std::uint64_t parse_duration_ns(const std::string &s) {
  std::size_t i = 0;
  while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.')) ++i;
  if (i == 0) return 0;
  const double val = std::strtod(s.substr(0, i).c_str(), nullptr);
  const std::string u = s.substr(i);
  double mult;
  if (u.empty() || u == "s") mult = 1e9;
  else if (u == "ms") mult = 1e6;
  else if (u == "us") mult = 1e3;
  else if (u == "ns") mult = 1.0;
  else if (u == "m") mult = 60e9;
  else if (u == "h") mult = 3600e9;
  else return 0;
  return static_cast<std::uint64_t>(val * mult);
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
  m_attr_leaf = 0; // plain leaf lines, matching the tunnel transcript
  m_attr_warn = color_id(COLOR_YELLOW, have_color, &next);
  // ANSI SGR fg colours (30-37) for interpreting device CLI output (cec_cli).
  for (short c = 0; c < 8; ++c)
    m_ansi_fg[c] = color_id(c, have_color, &next);

  // Mouse-wheel scrolling of the transcript. Important inside tmux, where the
  // wheel otherwise drives tmux copy-mode instead of this viewport.
  mousemask(BUTTON4_PRESSED | BUTTON5_PRESSED, nullptr);
  mouseinterval(0);

  relayout();

  update_sink::instance().add(
      [this](const std::string &line) { this->println(line); });
  println("Ready. Waiting for a device to open DialTcc.Subscribe …  (type 'help')");

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
  if (m_foot) delwin(m_foot);
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
  s += "      PgUp/PgDn·←/→ scroll · ^D quit";
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
  char hdr[96];
  std::snprintf(hdr, sizeof(hdr), " %-8s %-22s %s", "SESSION", "DEVICE", "UPTIME");
  mvwaddnstr(m_sessions, 0, 0, hdr, utf8_clip(hdr, w));
  wattroff(m_sessions, A_DIM);

  auto snap = mgmt_hub::instance().snapshot();
  const std::time_t now = std::time(nullptr);
  const int rows = th - 1;
  for (int i = 0; i < static_cast<int>(snap.size()) && i < rows; ++i) {
    char sess[16];
    std::snprintf(sess, sizeof(sess), "#%d", snap[i].id);
    char line[192];
    std::snprintf(line, sizeof(line), " %-8s %-22.22s %s", sess,
                  snap[i].device.empty() ? "-" : snap[i].device.c_str(),
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

// Session → device summary (the mgmt analogue of the tunnel's target footer).
void mgmt_tui::draw_foot() {
  if (!m_foot) return;
  int h = 0, w = 0;
  getmaxyx(m_foot, h, w);
  (void)h;
  werase(m_foot);
  auto snap = mgmt_hub::instance().snapshot();
  const std::time_t now = std::time(nullptr);
  std::string s;
  if (snap.empty()) {
    s = " (no sessions — waiting for a device to open DialTcc.Subscribe)";
  } else {
    // The top pane is gone; the whole session summary lives here:
    // "#1 role(hostname)  3m".
    s = " ";
    for (std::size_t i = 0; i < snap.size(); ++i) {
      if (i) s += "   ·   ";
      const auto &t = snap[i];
      s += "#" + std::to_string(t.id) + " ";
      if (!t.role.empty() && !t.hostname.empty()) s += t.role + "(" + t.hostname + ")";
      else if (!t.hostname.empty()) s += t.hostname;
      else if (!t.role.empty()) s += t.role;
      else if (!t.device.empty()) s += t.device;
      else s += "(probing…)";
      s += "  " + fmt_uptime(now - t.since);
    }
  }
  // Sticky CliRequest settings moved off the header live here.
  std::string set;
  if (m_cec) set += " cec_cli";
  if (m_json) set += " json";
  if (!m_timeout_disp.empty()) set += " " + m_timeout_disp;
  s += "      · " + (set.empty() ? std::string("defaults") : set);
  const int attr = (m_attr_reply ? m_attr_reply : A_NORMAL) | A_BOLD;
  wattron(m_foot, attr);
  mvwaddnstr(m_foot, 0, 0, s.c_str(), utf8_clip(s, w));
  wattroff(m_foot, attr);
  wnoutrefresh(m_foot);
}

void mgmt_tui::draw_input() {
  if (!m_inp) return;
  int bh = 0, bw = 0;
  getmaxyx(m_inp, bh, bw);
  (void)bh;
  werase(m_inp);

  // Claude-style rounded input box (matches the gnmi_peer TUI), softly dimmed.
  wattron(m_inp, A_DIM);
  mvwaddstr(m_inp, 0, 0, "\xE2\x95\xAD");      // ╭
  mvwaddstr(m_inp, 0, bw - 1, "\xE2\x95\xAE"); // ╮
  mvwaddstr(m_inp, 2, 0, "\xE2\x95\xB0");      // ╰
  mvwaddstr(m_inp, 2, bw - 1, "\xE2\x95\xAF"); // ╯
  for (int x = 1; x < bw - 1; ++x) {
    mvwaddstr(m_inp, 0, x, "\xE2\x94\x80"); // ─
    mvwaddstr(m_inp, 2, x, "\xE2\x94\x80");
  }
  mvwaddstr(m_inp, 1, 0, "\xE2\x94\x82");      // │
  mvwaddstr(m_inp, 1, bw - 1, "\xE2\x94\x82");
  wattroff(m_inp, A_DIM);

  // Prompt reflects the first identified session: "role(hostname)> ", falling
  // back to "hostname> ", "role> ", or the default "❯ " before the probe.
  std::string prompt = "\xE2\x9D\xAF "; // "❯ "
  for (const auto &s : mgmt_hub::instance().snapshot()) {
    std::string who;
    if (!s.role.empty() && !s.hostname.empty()) who = s.role + "(" + s.hostname + ")";
    else if (!s.hostname.empty()) who = s.hostname;
    else if (!s.role.empty()) who = s.role;
    if (!who.empty()) { prompt = who + "> "; break; }
  }
  const int text_col = 2; // inside the border, one space of padding
  const int pcols = utf8_cols(prompt);
  int avail = bw - text_col - pcols - 2; // keep a right margin inside the box
  if (avail < 0) avail = 0;
  std::string shown = m_input;
  if (static_cast<int>(shown.size()) > avail)
    shown = shown.substr(shown.size() - avail); // show the tail

  const int attr = (m_attr_reply ? m_attr_reply : A_NORMAL) | A_BOLD;
  wattron(m_inp, attr);
  mvwaddnstr(m_inp, 1, text_col, prompt.c_str(), utf8_clip(prompt, bw - text_col - 1));
  wattroff(m_inp, attr);
  mvwaddstr(m_inp, 1, text_col + pcols, shown.c_str());
  int cursx = text_col + pcols + static_cast<int>(shown.size());
  if (cursx > bw - 2) cursx = bw - 2;
  wmove(m_inp, 1, cursx);
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
  if (m_lines.size() > 200000) m_lines.pop_front(); // deep scrollback
}

// Update the current ncurses attribute from a ";"-separated SGR parameter list.
// Reset (0 / empty) returns to base_attr (the line's own colour); 30-37/90-97
// set the fg colour; 1/2 bold/dim. Backgrounds and unknown codes are ignored.
void mgmt_tui::apply_sgr(const std::string &params, int &cur, int base) const {
  if (params.empty()) { cur = base; return; }
  std::istringstream ps(params);
  std::string tok;
  while (std::getline(ps, tok, ';')) {
    const int code = tok.empty() ? 0 : std::atoi(tok.c_str());
    if (code == 0) cur = base;
    else if (code == 1) cur |= A_BOLD;
    else if (code == 2) cur |= A_DIM;
    else if (code == 22) cur &= ~(A_BOLD | A_DIM);
    else if (code == 39) cur &= ~A_COLOR;
    else if (code >= 30 && code <= 37) cur = (cur & ~A_COLOR) | m_ansi_fg[code - 30];
    else if (code >= 90 && code <= 97)
      cur = ((cur & ~A_COLOR) | m_ansi_fg[code - 90]) | A_BOLD;
  }
}

void mgmt_tui::draw_ansi_line(int row, const std::string &s, int start_col,
                              int max_cols, int base_attr) {
  int cur = base_attr;
  int col = 0, printed = 0;
  const std::size_t n = s.size();
  for (std::size_t i = 0; i < n && printed < max_cols;) {
    if (s[i] == '\x1b') {
      if (i + 1 < n && s[i + 1] == '[') { // CSI
        std::size_t j = i + 2;
        while (j < n && !std::isalpha((unsigned char)s[j])) ++j;
        if (j < n && s[j] == 'm')
          apply_sgr(s.substr(i + 2, j - (i + 2)), cur, base_attr);
        i = (j < n) ? j + 1 : n;
      } else if (i + 1 < n && s[i + 1] == ']') { // OSC
        i += 2;
        while (i < n && s[i] != '\x07') ++i;
        if (i < n) ++i;
      } else {
        ++i;
      }
      continue;
    }
    const unsigned char c = static_cast<unsigned char>(s[i]);
    int len = (c & 0x80) == 0 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3
              : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (i + len > n) len = 1;
    if (col >= start_col) {
      if (cur) wattron(m_out, cur);
      mvwaddnstr(m_out, row, printed, s.c_str() + i, len);
      if (cur) wattroff(m_out, cur);
      ++printed;
    }
    ++col;
    i += len;
  }
}

void mgmt_tui::redraw_out() {
  if (!m_out) return;
  int H = 0, W = 0;
  getmaxyx(m_out, H, W);
  werase(m_out);
  const int total = static_cast<int>(m_lines.size());
  const int view_h = H - 1; // bottom row is the horizontal scrollbar
  int maxscroll = total - view_h;
  if (maxscroll < 0) maxscroll = 0;
  if (m_scroll > maxscroll) m_scroll = maxscroll;
  if (m_scroll < 0) m_scroll = 0;
  int first = total - view_h - m_scroll;
  if (first < 0) first = 0;
  const bool vbar = (W > 2 && total > view_h);
  const int text_w = vbar ? W - 1 : W;

  // Widest visible line → bound horizontal scroll + size the h-scrollbar.
  int maxw = 0;
  for (int row = 0; row < view_h; ++row) {
    const int idx = first + row;
    if (idx < 0 || idx >= total) break;
    const int lw = visible_cols(m_lines[idx]); // printable cols (ignore ANSI)
    if (lw > maxw) maxw = lw;
  }
  int maxh = maxw - text_w;
  if (maxh < 0) maxh = 0;
  if (m_hscroll > maxh) m_hscroll = maxh;
  if (m_hscroll < 0) m_hscroll = 0;

  for (int row = 0; row < view_h; ++row) {
    const int idx = first + row;
    if (idx < 0 || idx >= total) break;
    const std::string &l = m_lines[idx];
    draw_ansi_line(row, l, m_hscroll, text_w, attr_for(l));
  }

  // Vertical scrollbar (right column, content rows only).
  if (vbar) {
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

  // Horizontal scrollbar on the bottom row (thumb fills the track if it fits).
  {
    int thumb = maxw > 0 ? text_w * text_w / maxw : text_w;
    if (thumb < 1) thumb = 1;
    if (thumb > text_w) thumb = text_w;
    const int track = text_w - thumb;
    int thumb_left = maxh > 0 ? track * m_hscroll / maxh : 0;
    if (thumb_left < 0) thumb_left = 0;
    if (thumb_left > track) thumb_left = track;
    wattron(m_out, A_DIM);
    for (int x = 0; x < text_w; ++x)
      mvwaddstr(m_out, view_h, x,
                (x >= thumb_left && x < thumb_left + thumb) ? "\xE2\x96\x84"   // ▄
                                                            : "\xE2\x94\x80"); // ─
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
  draw_foot();
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
  // Record in command history (skip consecutive duplicates); reset the cursor.
  if (m_history.empty() || m_history.back() != line)
    m_history.push_back(line);
  m_hist_idx = static_cast<int>(m_history.size());
  if (line == "quit" || line == "exit") {
    event_base_loopbreak(evt_base::instance().get());
    return;
  }
  if (line == "help" || line == "?") {
    println("commands:");
    println("  <cmd> [args]                    CLI command on the BN");
    println("  gnmi get|set|subscribe <spec>   gNMI over the dial-out");
    println("  send <file.lua>                 DeviceRequest described in Lua");
    println("  @<device_id> <cmd> …            target a specific device");
    println("  :set cec_cli|json on|off · :set timeout 20s · :show · :reset");
    println("  Up/Down history · PgUp/PgDn/Home, Shift+Up/Down (1 line), ←/→ "
            "scroll · quit/^D leave");
    return;
  }

  // ':' commands manage sticky settings (cec/json/timeout) — device_id is
  // inline per command via a leading @<id>.
  if (line[0] == ':') {
    std::istringstream ms(line.substr(1));
    std::string verb, key, val;
    ms >> verb >> key >> val;
    auto on_off = [&](bool &flag) {
      flag = (val == "on" || val == "1" || val == "true" || val == "yes");
    };
    if (verb == "set" && (key == "cec_cli" || key == "cec")) { on_off(m_cec); }
    else if (verb == "set" && key == "json") { on_off(m_json); }
    else if (verb == "set" && key == "timeout") {
      const std::uint64_t ns = parse_duration_ns(val);
      if (ns == 0 && val != "0") { println("[mgmt] bad timeout '" + val + "'"); return; }
      m_timeout_ns = ns; m_timeout_disp = (ns ? val : "");
    }
    else if (verb == "reset") {
      m_cec = m_json = false; m_timeout_ns = 0; m_timeout_disp.clear();
    }
    else if (verb == "show") { /* fall through to the summary below */ }
    else { println("[mgmt] unknown ':' command (set cec_cli|json|timeout, show, reset)"); return; }
    println(std::string("[mgmt] settings: cec_cli=") + (m_cec ? "on" : "off") +
            " json=" + (m_json ? "on" : "off") + " timeout=" +
            (m_timeout_disp.empty() ? "default" : m_timeout_disp));
    draw_header();
    doupdate();
    return;
  }

  // `send <file.lua>` builds a whole DeviceRequest from a Lua file via reflection.
  if (line.rfind("send ", 0) == 0) {
    send_file(trim(line.substr(5)));
    return;
  }

  // Optional leading @<device_id> targets a specific device (empty => the BN).
  std::istringstream ss(line);
  std::string tok, device_id;
  ss >> tok;
  if (!tok.empty() && tok[0] == '@') {
    device_id = tok.substr(1);
    ss >> tok; // the command itself
  }
  const std::string cmd = tok;
  std::vector<std::string> args;
  std::string a;
  while (ss >> a) args.push_back(a);
  if (cmd.empty()) { println("[mgmt] no command after @" + device_id); return; }

  // `gnmi <verb> <spec>` builds a gNMI request (Get/Set/Subscribe) instead of a
  // CliRequest; the device unpacks it from DeviceRequest.request.
  if (cmd == "gnmi") {
    if (args.empty()) { println("[mgmt] usage: gnmi get|set|subscribe <spec>"); return; }
    const std::string verb = args[0];
    std::string spec;
    for (std::size_t i = 1; i < args.size(); ++i) {
      if (i > 1) spec += " ";
      spec += args[i];
    }
    send_gnmi(verb, spec, device_id);
    return;
  }

  const std::string rpc_id = mgmt_hub::instance().send_cli(
      cmd, args, device_id, m_cec, m_json, m_timeout_ns);
  if (rpc_id.empty())
    println("[mgmt] no session connected — not sent: '" + line + "'");
  else
    println("[mgmt] \xE2\x86\x92 " + (device_id.empty() ? "" : "@" + device_id + " ") +
            "'" + cmd + "'  rpc=" + rpc_id); // →
}

void mgmt_tui::send_gnmi(const std::string &verb, const std::string &spec,
                         const std::string &device_id) {
  std::string rpc_id, label;
  auto send = [&](const google::protobuf::Message &m) {
    rpc_id = mgmt_hub::instance().send_request(m, device_id, label);
  };

  if (verb == "get") {
    if (spec.empty()) { println("[mgmt] usage: gnmi get <xpath>[,<xpath>...]"); return; }
    gnmi::GetRequest g;
    std::istringstream ps(spec);
    std::string p;
    int n = 0;
    while (std::getline(ps, p, ',')) {
      p = trim(p);
      if (p.empty()) continue;
      *g.add_path() = gnmi_util::parse_yang_path(p);
      ++n;
    }
    if (n == 0) { println("[mgmt] no valid xpaths"); return; }
    g.set_encoding(gnmi::JSON);
    label = "gnmi get " + spec;
    send(g);
  } else if (verb == "set") {
    if (spec.empty()) { println("[mgmt] usage: gnmi set <xpath>:<val>[,...]"); return; }
    gnmi::SetRequest s;
    std::istringstream ps(spec);
    std::string pair;
    int n = 0;
    while (std::getline(ps, pair, ',')) {
      pair = trim(pair);
      const auto colon = pair.find(':');
      if (colon == std::string::npos) continue;
      auto *u = s.add_update();
      *u->mutable_path() = gnmi_util::parse_yang_path(trim(pair.substr(0, colon)));
      gnmi_util::set_typed_value(u->mutable_val(), pair.substr(colon + 1));
      ++n;
    }
    if (n == 0) { println("[mgmt] no valid <xpath>:<value> pairs"); return; }
    label = "gnmi set " + spec;
    send(s);
  } else if (verb == "subscribe" || verb == "sub") {
    if (spec.empty()) { println("[mgmt] usage: gnmi subscribe <xpath>[,...] [interval]"); return; }
    // Optional trailing interval token => SAMPLE.
    std::string paths = spec;
    std::uint64_t interval_ns = 0;
    if (const auto ws = spec.find_last_of(" \t"); ws != std::string::npos) {
      const std::string tail = trim(spec.substr(ws + 1));
      if (!tail.empty() && std::isdigit((unsigned char)tail[0])) {
        interval_ns = parse_duration_ns(tail);
        if (interval_ns) paths = spec.substr(0, ws);
      }
    }
    gnmi::SubscribeRequest sub;
    auto *sl = sub.mutable_subscribe();
    sl->set_mode(gnmi::SubscriptionList::STREAM);
    sl->set_encoding(gnmi::JSON);
    std::istringstream ps(paths);
    std::string p;
    int n = 0;
    while (std::getline(ps, p, ',')) {
      p = trim(p);
      if (p.empty()) continue;
      auto *su = sl->add_subscription();
      *su->mutable_path() = gnmi_util::parse_yang_path(p);
      if (interval_ns) {
        su->set_mode(gnmi::SAMPLE);
        su->set_sample_interval(interval_ns);
      }
      ++n;
    }
    if (n == 0) { println("[mgmt] no valid xpaths"); return; }
    label = "gnmi subscribe " + paths;
    send(sub);
  } else {
    println("[mgmt] unknown gnmi verb '" + verb + "' (get|set|subscribe)");
    return;
  }

  if (rpc_id.empty())
    println("[mgmt] no session connected — not sent: " + label);
  else
    println("[mgmt] \xE2\x86\x92 " +
            (device_id.empty() ? "" : "@" + device_id + " ") + label +
            "  rpc=" + rpc_id);
}

void mgmt_tui::send_file(const std::string &path) {
  if (path.empty()) { println("[mgmt] usage: send <file.lua>"); return; }
  lua_file lf;
  lf.process_create_luafile(path);
  auto it = lf.commands().find(path);
  if (it == lf.commands().end()) {
    println("[mgmt] cannot load '" + path + "' (not found or not a table)");
    return;
  }
  tnmi::DeviceRequest req;
  std::string err;
  if (!lua_proto::populate(it->second, &req, err)) {
    println("[mgmt] " + path + ": " + err);
    return;
  }
  const std::string label = "send " + path;
  const std::string rpc_id = mgmt_hub::instance().send_device_request(req, label);
  if (rpc_id.empty())
    println("[mgmt] no session connected — not sent: " + path);
  else
    println("[mgmt] \xE2\x86\x92 " + label +
            (req.device_id().empty() ? "" : "  dev=" + req.device_id()) +
            "  rpc=" + rpc_id);
}

void mgmt_tui::relayout() {
  int H = 0, W = 0;
  getmaxyx(stdscr, H, W);
  if (H < 7 || W < 10) return;

  // No top pane — the transcript fills the whole area; session/identity live in
  // the footer next to the input box: header(1) + transcript + footer(1) + box(3).
  int out_h = H - 1 - 1 - 3;
  if (out_h < 1) out_h = 1;

  if (m_head) { delwin(m_head); m_head = nullptr; }
  if (m_sessions) { delwin(m_sessions); m_sessions = nullptr; }
  if (m_sep) { delwin(m_sep); m_sep = nullptr; }
  if (m_out) { delwin(m_out); m_out = nullptr; }
  if (m_foot) { delwin(m_foot); m_foot = nullptr; }
  if (m_inp) { delwin(m_inp); m_inp = nullptr; }

  m_head = newwin(1, W, 0, 0);
  m_out = newwin(out_h, W, 1, 0);
  m_foot = newwin(1, W, H - 4, 0);
  m_inp = newwin(3, W, H - 3, 0);
  scrollok(m_out, FALSE);
  keypad(m_inp, TRUE);
  nodelay(m_inp, TRUE);

  clear();
  refresh();
  draw_header();
  redraw_out();
  draw_foot();
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
  self->draw_foot();
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
    } else if (ch == KEY_SR) {     // Shift+Up → scroll up one line
      scroll_by(1);
    } else if (ch == KEY_SF) {     // Shift+Down → scroll down one line
      scroll_by(-1);
    } else if (ch == KEY_UP) {
      // Recall the previous command from history into the input line.
      if (!m_history.empty() && m_hist_idx > 0) {
        --m_hist_idx;
        m_input = m_history[m_hist_idx];
        draw_input();
        doupdate();
      }
    } else if (ch == KEY_DOWN) {
      // Move forward in history; past the newest entry clears the line.
      if (m_hist_idx < static_cast<int>(m_history.size())) {
        ++m_hist_idx;
        m_input = (m_hist_idx < static_cast<int>(m_history.size()))
                      ? m_history[m_hist_idx]
                      : std::string();
        draw_input();
        doupdate();
      }
    } else if (ch == KEY_HOME) {
      m_scroll = static_cast<int>(m_lines.size());
      redraw_out(); draw_input(); doupdate();
    } else if (ch == KEY_END) {
      m_scroll = 0;
      redraw_out(); draw_input(); doupdate();
    } else if (ch == KEY_LEFT) {
      m_hscroll -= 8; if (m_hscroll < 0) m_hscroll = 0;
      redraw_out(); draw_input(); doupdate();
    } else if (ch == KEY_RIGHT) {
      m_hscroll += 8; // clamped to content width in redraw_out
      redraw_out(); draw_input(); doupdate();
    } else if (ch == KEY_MOUSE) {
      MEVENT ev;
      if (getmouse(&ev) == OK) {
        if (ev.bstate & BUTTON4_PRESSED) scroll_by(3);       // wheel up
        else if (ev.bstate & BUTTON5_PRESSED) scroll_by(-3); // wheel down
      }
    } else if (ch >= 32 && ch < 127) { // printable → append to the command
      m_input.push_back(static_cast<char>(ch));
      draw_input();
      doupdate();
    }
  }
  return 0;
}
