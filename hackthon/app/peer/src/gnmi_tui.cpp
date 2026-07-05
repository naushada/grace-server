#include "gnmi_tui.hpp"

#include <ncurses.h>
#include <unistd.h> // STDIN_FILENO

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
// ---------------------------------------------------------------------------

gnmi_tui::gnmi_tui(const endpoint &remote, const tls_config &tls,
                   const palette_config &colors)
    : evt_io(STDIN_FILENO, rawfd_tag{}),
      m_cmd(remote, tls,
            [this](const std::string &line) { println(line); }) {
  initscr();
  cbreak();
  noecho();

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

  int rows = 0, cols = 0;
  getmaxyx(stdscr, rows, cols);
  if (rows < 3)
    rows = 3;

  // Row 0: input line. Row 1: divider. Rows 2..end: scrolling output.
  m_input_win = newwin(1, cols, 0, 0);
  m_output_win = newwin(rows - 2, cols, 2, 0);
  keypad(m_input_win, TRUE);
  nodelay(m_input_win, TRUE);
  scrollok(m_output_win, TRUE);
  idlok(m_output_win, TRUE);

  mvhline(1, 0, ACS_HLINE, cols);
  refresh();

  println("gnmi_peer ready — remote " + remote.host + ":" +
          std::to_string(remote.port));
  println("commands: gnmi set <xpath>:<val>[,<xpath>:<val>]  |  "
          "gnmi get <xpath>[,<xpath>]  |  help  |  quit");
  draw_input();
}

gnmi_tui::~gnmi_tui() {
  if (m_input_win)
    delwin(m_input_win);
  if (m_output_win)
    delwin(m_output_win);
  endwin();
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void gnmi_tui::draw_input() {
  werase(m_input_win);
  mvwprintw(m_input_win, 0, 0, "Marvel> %s", m_line.c_str());
  wrefresh(m_input_win);
}

// Classify an output line by its leading tag and return its resolved attribute.
int gnmi_tui::attr_for(const std::string &s) const {
  auto starts = [&](const char *p) { return s.rfind(p, 0) == 0; };
  if (starts("[remote]"))
    return m_attr_remote;
  if (starts("[set] OK") || starts("[get] OK"))
    return m_attr_ok;
  if (starts("Marvel> "))
    return m_attr_echo;
  if (s.find("error") != std::string::npos ||
      s.find("denied") != std::string::npos ||
      s.find("FAIL") != std::string::npos || starts("unknown command") ||
      starts("usage") || starts("no valid") || starts("  skip"))
    return m_attr_warn;
  return 0; // terminal default
}

void gnmi_tui::println(const std::string &line) {
  std::istringstream ss(line);
  std::string part;
  bool any = false;
  while (std::getline(ss, part, '\n')) {
    const int attr = attr_for(part);
    if (attr)
      wattron(m_output_win, attr);
    waddstr(m_output_win, part.c_str());
    if (attr)
      wattroff(m_output_win, attr);
    waddch(m_output_win, '\n');
    any = true;
  }
  if (!any)
    waddch(m_output_win, '\n');
  wrefresh(m_output_win);
  // Return the cursor to the input line so typing stays visible.
  draw_input();
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
  while ((ch = wgetch(m_input_win)) != ERR) {
    if (ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
      const std::string cmd = m_line;
      m_line.clear();
      draw_input();
      if (!trim(cmd).empty())
        dispatch(cmd);
    } else if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
      if (!m_line.empty())
        m_line.pop_back();
      draw_input();
    } else if (ch == 4) { // Ctrl-D → quit
      event_base_loopbreak(evt_base::instance().get());
      return 0;
    } else if (ch >= 32 && ch < 127) {
      m_line.push_back(static_cast<char>(ch));
      draw_input();
    }
    // Other keys (arrows, function keys) are ignored in this first cut.
  }
  return 0;
}

// ---------------------------------------------------------------------------
// Command dispatch — echo into the pane, then delegate to the shared core.
// ---------------------------------------------------------------------------

void gnmi_tui::dispatch(const std::string &line) {
  println("Marvel> " + trim(line));
  if (!m_cmd.dispatch(line))
    event_base_loopbreak(evt_base::instance().get());
}
