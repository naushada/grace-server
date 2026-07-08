// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#ifndef __ansi_util_hpp__
#define __ansi_util_hpp__

#include <cctype>
#include <string>

// Remove ANSI/VT escape sequences (CSI "ESC[ … letter", OSC "ESC] … BEL/ST",
// stray ESC) from a string. Used to keep log files plain-text while the TUIs
// render device CLI colour.
inline std::string strip_ansi(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size();) {
    if (s[i] == '\x1b') {
      if (i + 1 < s.size() && s[i + 1] == '[') { // CSI
        i += 2;
        while (i < s.size() && !std::isalpha((unsigned char)s[i])) ++i;
        if (i < s.size()) ++i; // drop the final letter
      } else if (i + 1 < s.size() && s[i + 1] == ']') { // OSC → BEL or ST
        i += 2;
        while (i < s.size() && s[i] != '\x07' &&
               !(s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '\\'))
          ++i;
        if (i < s.size() && s[i] == '\x07') ++i;
        else if (i + 1 < s.size()) i += 2;
      } else {
        ++i; // lone ESC
      }
    } else {
      out += s[i++];
    }
  }
  return out;
}

#endif // __ansi_util_hpp__
