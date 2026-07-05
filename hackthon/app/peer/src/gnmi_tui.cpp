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

  int H = 0, W = 0;
  getmaxyx(stdscr, H, W);
  int out_h = H - 5; // header(1) + box(3) + hint(1)
  if (out_h < 1)
    out_h = 1;

  m_head = newwin(1, W, 0, 0);
  m_out = newwin(out_h, W, 1, 0);
  m_box = newwin(3, W, H - 4, 0);
  m_hint = newwin(1, W, H - 1, 0);

  scrollok(m_out, TRUE);
  idlok(m_out, TRUE);
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
  mvwaddstr(m_hint, 0, 3, "set · get · help · quit");
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
  wmove(m_box, 1, cursx);
  wrefresh(m_box);
}

// Classify an output line by its leading tag and return its resolved attribute.
int gnmi_tui::attr_for(const std::string &s) const {
  auto starts = [&](const char *p) { return s.rfind(p, 0) == 0; };
  if (starts("[remote]") || starts("[sub] {"))
    return m_attr_remote;
  if (starts("[set] OK") || starts("[get] OK"))
    return m_attr_ok;
  if (starts("❯"))
    return m_attr_echo;
  if (s.find("error") != std::string::npos ||
      s.find("denied") != std::string::npos ||
      s.find("FAIL") != std::string::npos || starts("unknown command") ||
      starts("usage") || starts("no valid") || starts("  skip"))
    return m_attr_warn;
  return 0; // terminal default
}

// Draw one already-split transcript line into m_out (no buffering, no refresh).
void gnmi_tui::render_line(const std::string &part) {
  const int attr = attr_for(part);
  if (attr)
    wattron(m_out, attr);
  waddstr(m_out, part.c_str());
  if (attr)
    wattroff(m_out, attr);
  waddch(m_out, '\n');
}

// Buffer a transcript line so it can be replayed after a resize. Capped so a
// long-running session (constant telemetry) does not grow without bound.
void gnmi_tui::push_history(const std::string &part) {
  m_lines.push_back(part);
  if (m_lines.size() > 1000)
    m_lines.pop_front();
}

void gnmi_tui::println(const std::string &line) {
  std::istringstream ss(line);
  std::string part;
  bool any = false;
  while (std::getline(ss, part, '\n')) {
    push_history(part);
    render_line(part);
    any = true;
  }
  if (!any) {
    push_history("");
    render_line("");
  }
  wrefresh(m_out);
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

  scrollok(m_out, TRUE);
  idlok(m_out, TRUE);
  keypad(m_box, TRUE);
  nodelay(m_box, TRUE);

  clear();
  refresh();
  draw_chrome();
  for (const auto &l : m_lines)
    render_line(l);
  wrefresh(m_out);
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
      if (!trim(cmd).empty())
        dispatch(cmd);
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
      if (!m_line.empty())
        m_line.pop_back();
      draw_box();
    } else if (ch == 4) { // Ctrl-D → quit
      event_base_loopbreak(evt_base::instance().get());
      return 0;
    } else if (ch == KEY_RESIZE) { // resize delivered via input path
      relayout();
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
