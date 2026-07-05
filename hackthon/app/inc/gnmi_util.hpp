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

#include <cstdio>
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

// ---------------------------------------------------------------------------
// JSON rendering (used to display Set-pushed ops and Subscribe notifications)
// ---------------------------------------------------------------------------

// Escape a string for embedding inside a JSON string literal.
inline std::string json_escape(const std::string &s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '"': o += "\\\""; break;
    case '\\': o += "\\\\"; break;
    case '\n': o += "\\n"; break;
    case '\r': o += "\\r"; break;
    case '\t': o += "\\t"; break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        char buf[8];
        std::snprintf(buf, sizeof(buf), "\\u%04x",
                      static_cast<unsigned>(static_cast<unsigned char>(c)));
        o += buf;
      } else {
        o += c;
      }
    }
  }
  return o;
}

// Render a TypedValue as a JSON value. json_val / json_ietf_val are already
// JSON text and emitted verbatim; scalars become JSON numbers/booleans;
// strings are quoted and escaped.
inline std::string typed_value_to_json(const gnmi::TypedValue &v) {
  switch (v.value_case()) {
  case gnmi::TypedValue::kStringVal:
    return "\"" + json_escape(v.string_val()) + "\"";
  case gnmi::TypedValue::kIntVal:
    return std::to_string(v.int_val());
  case gnmi::TypedValue::kUintVal:
    return std::to_string(v.uint_val());
  case gnmi::TypedValue::kBoolVal:
    return v.bool_val() ? "true" : "false";
  case gnmi::TypedValue::kDoubleVal:
    return std::to_string(v.double_val());
  case gnmi::TypedValue::kJsonVal:
    return v.json_val().empty() ? "null" : v.json_val();
  case gnmi::TypedValue::kJsonIetfVal:
    return v.json_ietf_val().empty() ? "null" : v.json_ietf_val();
  case gnmi::TypedValue::kAsciiVal:
    return "\"" + json_escape(v.ascii_val()) + "\"";
  default:
    return "null";
  }
}

// {"path":"/a/b","val":<json>}
inline std::string update_to_json(const gnmi::Update &u) {
  return "{\"path\":\"" + json_escape(path_to_string(u.path())) +
         "\",\"val\":" + typed_value_to_json(u.val()) + "}";
}

// One Set-pushed operation: {"op":"UPDATE","path":"/a/b","val":<json>}.
// val==nullptr (DELETE) omits the "val" field.
inline std::string op_to_json(const std::string &op, const gnmi::Path &path,
                              const gnmi::TypedValue *val) {
  std::string s = "{\"op\":\"" + op + "\",\"path\":\"" +
                  json_escape(path_to_string(path)) + "\"";
  if (val)
    s += ",\"val\":" + typed_value_to_json(*val);
  s += "}";
  return s;
}

// Notification -> {"timestamp":..,"prefix":"..","update":[..],"delete":[..]}
inline std::string notification_to_json(const gnmi::Notification &n) {
  std::string s = "{\"timestamp\":" + std::to_string(n.timestamp());
  const std::string pfx = path_to_string(n.prefix());
  if (pfx != "/")
    s += ",\"prefix\":\"" + json_escape(pfx) + "\"";
  s += ",\"update\":[";
  for (int i = 0; i < n.update_size(); ++i) {
    if (i)
      s += ",";
    s += update_to_json(n.update(i));
  }
  s += "]";
  if (n.delete__size() > 0) {
    s += ",\"delete\":[";
    for (int i = 0; i < n.delete__size(); ++i) {
      if (i)
        s += ",";
      s += "\"" + json_escape(path_to_string(n.delete_(i))) + "\"";
    }
    s += "]";
  }
  s += "}";
  return s;
}

// SubscribeResponse -> JSON: {"update":<notif>} | {"syncResponse":true} | {"error":..}
inline std::string
subscribe_response_to_json(const gnmi::SubscribeResponse &r) {
  switch (r.response_case()) {
  case gnmi::SubscribeResponse::kUpdate:
    return "{\"update\":" + notification_to_json(r.update()) + "}";
  case gnmi::SubscribeResponse::kSyncResponse:
    return std::string("{\"syncResponse\":") +
           (r.sync_response() ? "true" : "false") + "}";
  case gnmi::SubscribeResponse::kError:
    return "{\"error\":\"" + json_escape(r.error().message()) + "\"}";
  default:
    return "{}";
  }
}

} // namespace gnmi_util

#endif // __gnmi_util_hpp__
