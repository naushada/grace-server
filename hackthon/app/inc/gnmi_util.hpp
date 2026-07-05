#ifndef __gnmi_util_hpp__
#define __gnmi_util_hpp__

// Shared, header-only gNMI path / value helpers.
//
// These are used both by the local Set handler (client_app.cpp, to render
// received operations) and by the gnmi_peer TUI (to build outgoing SetRequest /
// GetRequest). Keeping one implementation here avoids the copies that currently
// live in app/cli/src/readline.cpp.
//
// All functions are `inline` so the header may be included by multiple
// translation units without ODR violations.

#include "gnmi/gnmi.pb.h"

#include <sstream>
#include <string>

namespace gnmi_util {

// Convert a YANG instance-identifier string such as
//   "/interfaces/interface[name=eth0]/state/oper-status"
// into a gnmi::Path with one PathElem per '/'-separated segment. Bracketed
// predicates "[key=val][key2=val2]" become PathElem.key map entries.
inline gnmi::Path parse_yang_path(const std::string &path_str) {
  gnmi::Path path;
  std::string s = path_str;
  if (!s.empty() && s.front() == '/')
    s = s.substr(1);
  if (s.empty())
    return path;

  std::istringstream ss(s);
  std::string segment;
  while (std::getline(ss, segment, '/')) {
    if (segment.empty())
      continue;
    auto *elem = path.add_elem();
    const auto bracket = segment.find('[');
    if (bracket == std::string::npos) {
      elem->set_name(segment);
    } else {
      elem->set_name(segment.substr(0, bracket));
      std::string rest = segment.substr(bracket);
      size_t pos = 0;
      while (pos < rest.size() && rest[pos] == '[') {
        const auto end_b = rest.find(']', pos);
        if (end_b == std::string::npos)
          break;
        const std::string kv = rest.substr(pos + 1, end_b - pos - 1);
        const auto eq = kv.find('=');
        if (eq != std::string::npos)
          (*elem->mutable_key())[kv.substr(0, eq)] = kv.substr(eq + 1);
        pos = end_b + 1;
      }
    }
  }
  return path;
}

// Render a gnmi::Path back to "/name[k=v]/name/..." form for display.
inline std::string path_to_string(const gnmi::Path &p) {
  std::string out;
  for (const auto &elem : p.elem()) {
    out += '/';
    out += elem.name();
    for (const auto &kv : elem.key()) {
      out += '[';
      out += kv.first;
      out += '=';
      out += kv.second;
      out += ']';
    }
  }
  return out.empty() ? "/" : out;
}

// Populate a gnmi::TypedValue from a string and an optional encoding hint.
//   enc == "JSON_IETF"          -> json_ietf_val
//   enc == "JSON" or looks JSON -> json_val  (value begins with '{' or '[')
//   otherwise                   -> string_val
// The gnmi_peer `set` command passes enc="" so bare scalars become string_val
// while JSON objects/arrays are carried as json_val.
inline void set_typed_value(gnmi::TypedValue *val, const std::string &value_str,
                            const std::string &enc = "") {
  const bool is_json =
      !value_str.empty() &&
      (value_str.front() == '{' || value_str.front() == '[');

  if (enc == "JSON_IETF") {
    val->set_json_ietf_val(value_str);
  } else if (enc == "JSON" || is_json) {
    val->set_json_val(value_str);
  } else {
    val->set_string_val(value_str);
  }
}

// Render a gnmi::TypedValue oneof to a human-readable string for display.
inline std::string typed_value_to_string(const gnmi::TypedValue &v) {
  switch (v.value_case()) {
  case gnmi::TypedValue::kStringVal:
    return v.string_val();
  case gnmi::TypedValue::kIntVal:
    return std::to_string(v.int_val());
  case gnmi::TypedValue::kUintVal:
    return std::to_string(v.uint_val());
  case gnmi::TypedValue::kBoolVal:
    return v.bool_val() ? "true" : "false";
  case gnmi::TypedValue::kDoubleVal:
    return std::to_string(v.double_val());
  case gnmi::TypedValue::kJsonVal:
    return v.json_val();
  case gnmi::TypedValue::kJsonIetfVal:
    return v.json_ietf_val();
  case gnmi::TypedValue::kAsciiVal:
    return v.ascii_val();
  default:
    return "<value>";
  }
}

} // namespace gnmi_util

#endif // __gnmi_util_hpp__
