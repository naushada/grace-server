#ifndef __readline_cpp__
#define __readline_cpp__

#include "readline.hpp"
#include "openvpn_tunnel_client.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <mosquitto.h>

// ---------------------------------------------------------------------------
// Single filesystem monitor — owns the lua_file engine that the CLI queries.
// ---------------------------------------------------------------------------
static fs_app fs_mon("/app/command");

// MQTT client shared by all gnmi command handlers.
// Initialised in main() from MQTT_HOST / MQTT_PORT env vars.
// topic scheme:
//   CLI publishes to   "cli/<target_ip>"       payload = rpc_path '\0' proto_bytes
//   CLI subscribes to  "cli_resp/<target_ip>"  payload = rpc_path '\0' status '\0' grpc_message '\0' proto_bytes
static struct mosquitto *g_mosq = nullptr;

// State for the one outstanding request at any time (CLI is synchronous).
struct cli_resp_t {
  bool        received{false};
  std::string rpc_path;
  int         grpc_status{-1};
  std::string grpc_message;
  std::string proto_bytes;
};
static cli_resp_t g_cli_resp;

// mosquitto message callback — fires when cli_resp/<ip> delivers a response.
static void on_cli_response(struct mosquitto* /*mosq*/, void* /*ud*/,
                             const struct mosquitto_message *msg) {
  if (!msg || !msg->payload || msg->payloadlen <= 0) return;
  const char  *raw = static_cast<const char *>(msg->payload);
  const size_t sz  = static_cast<size_t>(msg->payloadlen);

  // Parse: rpc_path '\0' status_str '\0' grpc_message '\0' proto_bytes
  const char *s1 = static_cast<const char *>(std::memchr(raw, '\0', sz));
  if (!s1) return;
  g_cli_resp.rpc_path = std::string(raw, s1 - raw);

  const char *s2 = static_cast<const char *>(std::memchr(s1 + 1, '\0', sz - static_cast<size_t>(s1 + 1 - raw)));
  if (!s2) return;
  g_cli_resp.grpc_status = std::stoi(std::string(s1 + 1, s2 - s1 - 1));

  const char *s3 = static_cast<const char *>(std::memchr(s2 + 1, '\0', sz - static_cast<size_t>(s2 + 1 - raw)));
  if (!s3) return;
  g_cli_resp.grpc_message = std::string(s2 + 1, s3 - s2 - 1);

  g_cli_resp.proto_bytes = std::string(s3 + 1, sz - static_cast<size_t>(s3 + 1 - raw));
  g_cli_resp.received = true;
}

// Print a decoded gNMI response received over the MQTT return path.
static void print_mqtt_response(const cli_resp_t &r) {
  if (r.grpc_status < 0) {
    std::cout << "[gnmi] transport error: " << r.grpc_message << '\n';
    return;
  }
  if (r.grpc_status != 0) {
    std::cout << "[gnmi] error status=" << r.grpc_status;
    if (!r.grpc_message.empty()) std::cout << " message=" << r.grpc_message;
    std::cout << '\n';
    return;
  }
  std::string text;
  if (r.rpc_path == "/gnmi.gNMI/Get") {
    gnmi::GetResponse resp;
    if (resp.ParseFromString(r.proto_bytes))
      google::protobuf::TextFormat::PrintToString(resp, &text);
    std::cout << "[gnmi_get] OK\n" << text;
  } else {
    gnmi::SetResponse resp;
    if (resp.ParseFromString(r.proto_bytes))
      google::protobuf::TextFormat::PrintToString(resp, &text);
    std::cout << "[gnmi_set] OK\n" << text;
  }
}

static void mqtt_gnmi_publish(const std::string &target_ip,
                               const std::string &rpc_path,
                               const std::string &proto_bytes) {
  if (!g_mosq) {
    std::cout << "[mqtt] not connected — falling back to direct gRPC\n";
    return;
  }

  // Subscribe to response topic BEFORE publishing to avoid a race.
  const std::string resp_topic = "cli_resp/" + target_ip;
  g_cli_resp = {};
  mosquitto_message_callback_set(g_mosq, on_cli_response);
  mosquitto_subscribe(g_mosq, nullptr, resp_topic.c_str(), 0);
  mosquitto_loop(g_mosq, 10, 1);  // flush SUBSCRIBE

  // Publish the request.
  const std::string topic   = "cli/" + target_ip;
  const std::string payload = rpc_path + '\0' + proto_bytes;
  const int rc = mosquitto_publish(g_mosq, nullptr,
                                   topic.c_str(),
                                   static_cast<int>(payload.size()),
                                   payload.data(), 0, false);
  if (rc != MOSQ_ERR_SUCCESS) {
    std::cerr << "[mqtt] publish failed: " << mosquitto_strerror(rc) << '\n';
    mosquitto_unsubscribe(g_mosq, nullptr, resp_topic.c_str());
    return;
  }
  std::cout << "[mqtt] published " << proto_bytes.size()
            << "B proto → topic=" << topic << '\n';
  mosquitto_loop(g_mosq, 0, 1);  // flush PUBLISH

  // Wait up to 5 s for the response to arrive on cli_resp/<ip>.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!g_cli_resp.received) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      std::cout << "[gnmi] response timeout (5s)\n";
      break;
    }
    const int ms = static_cast<int>(
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
    mosquitto_loop(g_mosq, std::min(ms, 100), 1);
  }

  if (g_cli_resp.received)
    print_mqtt_response(g_cli_resp);

  mosquitto_unsubscribe(g_mosq, nullptr, resp_topic.c_str());
  mosquitto_message_callback_set(g_mosq, nullptr);
}

// ---------------------------------------------------------------------------
// Completion session state.
//
// Readline's completion model calls each generator repeatedly — once with
// state == 0 (fresh session) and then with increasing state until the
// generator returns NULL. We therefore build the full list of candidates on
// state == 0 and serve them one at a time thereafter.
// ---------------------------------------------------------------------------
static std::vector<std::string> s_command_matches;
static std::size_t s_command_idx = 0;

static std::vector<std::string> s_param_matches;
static std::size_t s_param_idx = 0;

// First token on the current input line (i.e. the command name the user has
// already typed). Captured by command_completion() before it delegates to
// param_*_generator so those generators know which lua table to query.
static std::string s_active_command;

// ---------------------------------------------------------------------------
// In-memory picture of how a loaded lua file is represented:
//
//   m_commands["StartDownlinkConnectionsTestRequest.lua"]
//     └── members["StartDownlinkConnectionsTestRequest"]   <-- top-level cmd
//           └── shared_ptr<table_type>
//                 ├── members["rate_mbps"]   = value_type(50)
//                 ├── members["device_id"]   = vector<value_type>{"1","2"}
//                 └── members["test_param"]  = shared_ptr<table_type>{ ... }
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Helpers — traverse the lua_file data structure.
// ---------------------------------------------------------------------------

// Every first-level key across every loaded .lua file is considered a valid
// "command" at the prompt. StartDownlinkConnectionsTestRequest is one such
// key (defined inside StartDownlinkConnectionsTestRequest.lua).
static std::vector<std::string> collect_all_command_names() {
  std::vector<std::string> out;
  for (const auto &[file_name, top_table] : fs_mon.lua_engine()->commands()) {
    (void)file_name;
    for (const auto &[cmd_name, entry] : top_table.members) {
      (void)entry;
      out.push_back(cmd_name);
    }
  }
  return out;
}

// Return the nested parameter table for the given command name, or nullptr
// if the command wasn't found / isn't a table.
static std::shared_ptr<lua_file::table_type>
find_command_table(const std::string &cmd_name) {
  for (const auto &[file_name, top_table] : fs_mon.lua_engine()->commands()) {
    (void)file_name;
    auto it = top_table.members.find(cmd_name);
    if (it == top_table.members.end())
      continue;
    if (auto *ptr =
            std::get_if<std::shared_ptr<lua_file::table_type>>(&it->second)) {
      return *ptr;
    }
  }
  return nullptr;
}

// Walk a dotted path (e.g. "test_param.duration_sec") inside root and return
// a pointer to the matching entry (or nullptr if any segment is missing).
static const lua_file::entry_type *
walk_dotted_path(const lua_file::table_type &root, const std::string &path) {
  const lua_file::table_type *cur = &root;
  const lua_file::entry_type *last = nullptr;

  std::stringstream ss(path);
  std::string seg;
  while (std::getline(ss, seg, '.')) {
    if (!cur)
      return nullptr;
    auto it = cur->members.find(seg);
    if (it == cur->members.end())
      return nullptr;
    last = &it->second;

    if (auto *sub =
            std::get_if<std::shared_ptr<lua_file::table_type>>(last)) {
      cur = (*sub) ? sub->get() : nullptr;
    } else {
      cur = nullptr;
    }
  }
  return last;
}

// Render a lua_file::value_type to a string — used when we want to surface
// current values as suggestions after `=`.
static std::string value_to_string(const lua_file::value_type &v) {
  std::ostringstream os;
  std::visit(
      [&](auto &&val) {
        using V = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<V, std::nullptr_t>) {
          os << "nil";
        } else if constexpr (std::is_same_v<V, bool>) {
          os << (val ? "true" : "false");
        } else {
          os << val;
        }
      },
      v);
  return os.str();
}

// ---------------------------------------------------------------------------
// readline UI wiring
// ---------------------------------------------------------------------------

void custom_display_matches(char **matches, int len, int max_len) {
  (void)len;
  std::cout << "\n";

  int column_width = max_len + 2;
  int screen_width = 80; // Ideally get this from ioctl(TIOCGWINSZ)
  int cols = screen_width / column_width;
  if (cols < 1)
    cols = 1;

  for (int i = 1; matches[i]; i++) {
    std::cout << std::left << std::setw(column_width) << matches[i];
    if (i % cols == 0)
      std::cout << "\n";
  }

  std::cout << "\n";
  rl_on_new_line();
}

void init_readline() {
  // Tell readline not to fall back to filename completion if we return no
  // matches — we are fully in charge of the match list.
  rl_attempted_completion_over = 1;
  rl_attempted_completion_function = command_completion;

  // Remove BOTH '.' and '=' from the word-break set so that a token like
  //   test_param.rate_mbps=50
  // is handed to the generator as one atomic word for parsing.
  rl_basic_word_break_characters = " \t\n\"\\'`@$><;|&{(";

  rl_completion_display_matches_hook = custom_display_matches;
  using_history();
}

// ---------------------------------------------------------------------------
// Generators
// ---------------------------------------------------------------------------

// Completes the first word of the line — the command name itself.
char *command_generator(const char *text, int state) {
  if (state == 0) {
    s_command_matches.clear();
    s_command_idx = 0;

    std::string prefix(text ? text : "");
    for (auto &name : collect_all_command_names()) {
      if (prefix.empty() ||
          name.compare(0, prefix.length(), prefix) == 0) {
        s_command_matches.push_back(std::move(name));
      }
    }
    std::sort(s_command_matches.begin(), s_command_matches.end());
  }

  if (s_command_idx < s_command_matches.size()) {
    return strdup(s_command_matches[s_command_idx++].c_str());
  }
  return nullptr;
}

// Completes parameter *names* after the command. Produces dotted paths such
// as "rate_mbps=", "device_id=", "test_param.", "test_param.duration_sec=".
char *param_name_generator(const char *text, int state) {
  if (state == 0) {
    s_param_matches.clear();
    s_param_idx = 0;

    auto table = find_command_table(s_active_command);
    if (!table)
      return nullptr;

    std::string filter(text ? text : "");
    get_sub_keys(*table, "", filter, s_param_matches);
    std::sort(s_param_matches.begin(), s_param_matches.end());
  }

  if (s_param_idx < s_param_matches.size()) {
    return strdup(s_param_matches[s_param_idx++].c_str());
  }
  return nullptr;
}

// Completes parameter *values* after the user has typed "key=". Suggests
// either the current value (for scalars) or any array element (for arrays).
char *param_value_generator(const char *text, int state) {
  if (state == 0) {
    s_param_matches.clear();
    s_param_idx = 0;

    std::string full(text ? text : "");
    auto eq = full.find('=');
    if (eq == std::string::npos)
      return nullptr;

    std::string key_path = full.substr(0, eq);
    std::string typed_val = full.substr(eq + 1);

    auto table = find_command_table(s_active_command);
    if (!table)
      return nullptr;

    const lua_file::entry_type *entry = walk_dotted_path(*table, key_path);
    if (!entry)
      return nullptr;

    if (auto *val = std::get_if<lua_file::value_type>(entry)) {
      if (std::holds_alternative<bool>(*val)) {
        s_param_matches.push_back(key_path + "=true");
        s_param_matches.push_back(key_path + "=false");
      } else {
        s_param_matches.push_back(key_path + "=" + value_to_string(*val));
      }
    } else if (auto *vec =
                   std::get_if<std::vector<lua_file::value_type>>(entry)) {
      for (auto &v : *vec) {
        s_param_matches.push_back(key_path + "=" + value_to_string(v));
      }
    }

    // Narrow by whatever the user has already typed after '='.
    if (!typed_val.empty()) {
      s_param_matches.erase(
          std::remove_if(s_param_matches.begin(), s_param_matches.end(),
                         [&](const std::string &m) {
                           auto mp = m.find('=');
                           if (mp == std::string::npos)
                             return true;
                           std::string v = m.substr(mp + 1);
                           return v.compare(0, typed_val.length(),
                                            typed_val) != 0;
                         }),
          s_param_matches.end());
    }
  }

  if (s_param_idx < s_param_matches.size()) {
    return strdup(s_param_matches[s_param_idx++].c_str());
  }
  return nullptr;
}

// Entry point readline calls every time TAB is pressed.
char **command_completion(const char *text, std::int32_t start,
                          std::int32_t end) {
  rl_attempted_completion_over = 1;
  (void)end;

  // Word #1 on the line → complete the command name.
  if (start == 0) {
    s_active_command.clear();
    return rl_completion_matches(text, command_generator);
  }

  // Past the first token. Capture the command name from the line buffer so
  // the param_*_generator functions know which lua table to query.
  std::string line(rl_line_buffer ? rl_line_buffer : "");
  std::istringstream ls(line);
  ls >> s_active_command;

  std::string what(text ? text : "");
  if (what.find('=') != std::string::npos) {
    return rl_completion_matches(text, param_value_generator);
  }
  return rl_completion_matches(text, param_name_generator);
}

// ---------------------------------------------------------------------------
// Protobuf glue — build a message by name and apply key=value args.
// ---------------------------------------------------------------------------
google::protobuf::Message *create_message_by_name(const std::string &cmd_name) {
  const google::protobuf::Descriptor *descriptor =
      google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(
          cmd_name);

  if (!descriptor) {
    std::cerr << "Descriptor not found for message: " << cmd_name << std::endl;
    return nullptr;
  }

  const google::protobuf::Message *prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(
          descriptor);

  if (!prototype) {
    std::cerr << "Prototype not found for descriptor: " << cmd_name
              << std::endl;
    return nullptr;
  }

  return prototype->New();
}

// ---------------------------------------------------------------------------
// YANG path helpers
// ---------------------------------------------------------------------------

// Convert a YANG instance-identifier string such as
//   "/interfaces/interface[name=eth0]/state/oper-status"
// into a gnmi::Path with one PathElem per segment.
static gnmi::Path parse_yang_path(const std::string &path_str) {
  gnmi::Path path;
  std::string s = path_str;
  if (!s.empty() && s[0] == '/')
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
      // Parse "[key=val][key2=val2]..." predicates
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

// Populate a gnmi::TypedValue from a string and an encoding hint.
// If value_str looks like JSON (starts with '{' or '[') and encoding is JSON*,
// use json_val/json_ietf_val; otherwise fall back to string_val.
static void set_typed_value(gnmi::TypedValue *val,
                             const std::string &value_str,
                             const std::string &encoding) {
  const bool is_json =
      !value_str.empty() &&
      (value_str.front() == '{' || value_str.front() == '[');

  if (encoding == "JSON_IETF") {
    val->set_json_ietf_val(value_str);
  } else if (encoding == "JSON" || is_json) {
    val->set_json_val(value_str);
  } else {
    val->set_string_val(value_str);
  }
}

// Map encoding name string to gnmi::Encoding enum value.
static gnmi::Encoding parse_encoding(const std::string &enc) {
  if (enc == "PROTO")     return gnmi::PROTO;
  if (enc == "JSON_IETF") return gnmi::JSON_IETF;
  if (enc == "ASCII")     return gnmi::ASCII;
  return gnmi::JSON; // default
}

// ---------------------------------------------------------------------------
// gNMI CLI command handlers
// ---------------------------------------------------------------------------
// Each handler extracts target/port/prefix/path/value from the parsed
// argument map, builds the appropriate gnmi proto, serialises it, calls
// gnmi_client::call(), and prints the result in text-format protobuf.
//
// Common argument keys:
//   target   — IP address or hostname of the peer gNMI device
//   port     — TCP port (default 9339)
//   prefix   — common YANG path prefix (default "/")
//   path     — specific leaf / subtree relative to prefix
//   value    — new value for SET/UPDATE/REPLACE
//   encoding — JSON (default), PROTO, JSON_IETF

static std::string get_arg(const std::map<std::string, std::string> &args,
                            const std::string &key,
                            const std::string &def = "") {
  auto it = args.find(key);
  return it != args.end() ? it->second : def;
}

// Print a gnmi_client::response.  On success, deserialise body_pb as MsgT
// and print it in text format.
template <typename MsgT>
static void print_gnmi_response(const gnmi_client::response &resp,
                                 const std::string &op) {
  if (resp.grpc_status < 0) {
    std::cout << "[" << op << "] transport error: " << resp.grpc_message
              << "\n";
    return;
  }
  if (resp.grpc_status != 0) {
    std::cout << "[" << op << "] gRPC error status=" << resp.grpc_status;
    if (!resp.grpc_message.empty())
      std::cout << " message=" << resp.grpc_message;
    std::cout << "\n";
    return;
  }
  MsgT msg;
  if (!msg.ParseFromString(resp.body_pb)) {
    std::cout << "[" << op << "] OK but response proto parse failed ("
              << resp.body_pb.size() << " bytes)\n";
    return;
  }
  std::string text;
  google::protobuf::TextFormat::PrintToString(msg, &text);
  std::cout << "[" << op << "] OK\n" << text;
}

// GET — retrieve one path from the target.
static void handle_gnmi_get(const std::map<std::string, std::string> &args) {
  const std::string host     = get_arg(args, "target", "127.0.0.1");
  const uint16_t    port     = static_cast<uint16_t>(std::stoi(get_arg(args, "port", "9339")));
  const std::string prefix   = get_arg(args, "prefix", "/");
  const std::string path_str = get_arg(args, "path", "");
  const std::string encoding = get_arg(args, "encoding", "JSON");
  const std::string role     = get_arg(args, "role", "VIEWER");

  gnmi::GetRequest req;
  // Always populate prefix so target (role) is transmitted to the server.
  {
    auto *pfx = req.mutable_prefix();
    if (prefix != "/" && !prefix.empty())
      *pfx = parse_yang_path(prefix);
    pfx->set_target(role);
  }
  *req.add_path() = parse_yang_path(path_str);
  req.set_encoding(parse_encoding(encoding));

  std::string req_pb;
  req.SerializeToString(&req_pb);

  std::cout << "[gnmi_get] target=" << host << " prefix=" << prefix
            << " path=" << path_str << "\n";
  mqtt_gnmi_publish(host, "/gnmi.gNMI/Get", req_pb);
}

// UPDATE — merge the value into the existing configuration at path.
// Requires role=ADMIN; routes through VPN tunnel when tunnel_host is set.
static void handle_gnmi_update(const std::map<std::string, std::string> &args) {
  const std::string host        = get_arg(args, "target", "127.0.0.1");
  const uint16_t    port        = static_cast<uint16_t>(std::stoi(get_arg(args, "port", "9339")));
  const std::string prefix      = get_arg(args, "prefix", "/");
  const std::string path_str    = get_arg(args, "path", "");
  const std::string value       = get_arg(args, "value", "");
  const std::string encoding    = get_arg(args, "encoding", "JSON");
  const std::string role        = get_arg(args, "role", "ADMIN");
  const std::string tunnel_host = get_arg(args, "tunnel_host", "");
  const uint16_t    tunnel_port = static_cast<uint16_t>(
      std::stoi(get_arg(args, "tunnel_port", "1194")));

  // Connect through VPN tunnel before sending the update.
  if (!tunnel_host.empty()) {
    const auto tr = openvpn_tunnel_client::connect(tunnel_host, tunnel_port);
    if (!tr.ok) {
      std::cout << "[gnmi_update] VPN tunnel error: " << tr.message << "\n";
      return;
    }
    std::cout << "[gnmi_update] via VPN assigned=" << tr.assigned_ip << "\n";
  }

  gnmi::SetRequest req;
  {
    auto *pfx = req.mutable_prefix();
    if (prefix != "/" && !prefix.empty())
      *pfx = parse_yang_path(prefix);
    pfx->set_target(role);
  }
  auto *upd = req.add_update();
  *upd->mutable_path() = parse_yang_path(path_str);
  set_typed_value(upd->mutable_val(), value, encoding);

  std::string req_pb;
  req.SerializeToString(&req_pb);

  std::cout << "[gnmi_update] target=" << host << " role=" << role
            << " prefix=" << prefix << " path=" << path_str << "\n";
  mqtt_gnmi_publish(host, "/gnmi.gNMI/Set", req_pb);
}

// REPLACE — completely replace the subtree at path with the given value.
// Requires role=ADMIN; routes through VPN tunnel when tunnel_host is set.
static void handle_gnmi_replace(const std::map<std::string, std::string> &args) {
  const std::string host        = get_arg(args, "target", "127.0.0.1");
  const uint16_t    port        = static_cast<uint16_t>(std::stoi(get_arg(args, "port", "9339")));
  const std::string prefix      = get_arg(args, "prefix", "/");
  const std::string path_str    = get_arg(args, "path", "");
  const std::string value       = get_arg(args, "value", "{}");
  const std::string encoding    = get_arg(args, "encoding", "JSON");
  const std::string role        = get_arg(args, "role", "ADMIN");
  const std::string tunnel_host = get_arg(args, "tunnel_host", "");
  const uint16_t    tunnel_port = static_cast<uint16_t>(
      std::stoi(get_arg(args, "tunnel_port", "1194")));

  if (!tunnel_host.empty()) {
    const auto tr = openvpn_tunnel_client::connect(tunnel_host, tunnel_port);
    if (!tr.ok) {
      std::cout << "[gnmi_replace] VPN tunnel error: " << tr.message << "\n";
      return;
    }
    std::cout << "[gnmi_replace] via VPN assigned=" << tr.assigned_ip << "\n";
  }

  gnmi::SetRequest req;
  {
    auto *pfx = req.mutable_prefix();
    if (prefix != "/" && !prefix.empty())
      *pfx = parse_yang_path(prefix);
    pfx->set_target(role);
  }
  auto *rep = req.add_replace();
  *rep->mutable_path() = parse_yang_path(path_str);
  set_typed_value(rep->mutable_val(), value, encoding);

  std::string req_pb;
  req.SerializeToString(&req_pb);

  std::cout << "[gnmi_replace] target=" << host << " role=" << role
            << " prefix=" << prefix << " path=" << path_str << "\n";
  mqtt_gnmi_publish(host, "/gnmi.gNMI/Set", req_pb);
}

// DELETE — remove the node at path from the target configuration.
// Requires role=ADMIN; routes through VPN tunnel when tunnel_host is set.
static void handle_gnmi_delete(const std::map<std::string, std::string> &args) {
  const std::string host        = get_arg(args, "target", "127.0.0.1");
  const uint16_t    port        = static_cast<uint16_t>(std::stoi(get_arg(args, "port", "9339")));
  const std::string prefix      = get_arg(args, "prefix", "/");
  const std::string path_str    = get_arg(args, "path", "");
  const std::string role        = get_arg(args, "role", "ADMIN");
  const std::string tunnel_host = get_arg(args, "tunnel_host", "");
  const uint16_t    tunnel_port = static_cast<uint16_t>(
      std::stoi(get_arg(args, "tunnel_port", "1194")));

  if (!tunnel_host.empty()) {
    const auto tr = openvpn_tunnel_client::connect(tunnel_host, tunnel_port);
    if (!tr.ok) {
      std::cout << "[gnmi_delete] VPN tunnel error: " << tr.message << "\n";
      return;
    }
    std::cout << "[gnmi_delete] via VPN assigned=" << tr.assigned_ip << "\n";
  }

  gnmi::SetRequest req;
  {
    auto *pfx = req.mutable_prefix();
    if (prefix != "/" && !prefix.empty())
      *pfx = parse_yang_path(prefix);
    pfx->set_target(role);
  }
  *req.add_delete_() = parse_yang_path(path_str);

  std::string req_pb;
  req.SerializeToString(&req_pb);

  std::cout << "[gnmi_delete] target=" << host << " role=" << role
            << " prefix=" << prefix << " path=" << path_str << "\n";
  mqtt_gnmi_publish(host, "/gnmi.gNMI/Set", req_pb);
}

// ---------------------------------------------------------------------------
// Command dispatcher
// ---------------------------------------------------------------------------

void process_command(const std::string &line) {
  std::stringstream ss(line);
  std::string cmd_name;
  ss >> cmd_name;

  // Accept either a filename key OR a first-level command key inside any
  // loaded file.
  bool found =
      fs_mon.lua_engine()->commands().find(cmd_name) !=
      fs_mon.lua_engine()->commands().end();
  if (!found) {
    for (const auto &[file_name, top_table] :
         fs_mon.lua_engine()->commands()) {
      (void)file_name;
      if (top_table.members.find(cmd_name) != top_table.members.end()) {
        found = true;
        break;
      }
    }
  }
  if (!found) {
    std::cout << "Unknown command: " << cmd_name << std::endl;
    return;
  }

  // Parse "key=value" pairs from the rest of the line.
  std::map<std::string, std::string> arguments;
  std::string pair;
  while (ss >> pair) {
    const size_t pos = pair.find('=');
    if (pos != std::string::npos)
      arguments[pair.substr(0, pos)] = pair.substr(pos + 1);
  }

  // gNMI commands: build proto, gRPC-frame, send to peer device.
  if (cmd_name == "gnmi_get") {
    handle_gnmi_get(arguments);
  } else if (cmd_name == "gnmi_update") {
    handle_gnmi_update(arguments);
  } else if (cmd_name == "gnmi_replace") {
    handle_gnmi_replace(arguments);
  } else if (cmd_name == "gnmi_delete") {
    handle_gnmi_delete(arguments);
  } else {
    // Generic protobuf command via reflection.
    apply_to_proto(cmd_name, arguments);
  }
}

void apply_to_proto(const std::string &cmd_name,
                    const std::map<std::string, std::string> &args) {
  google::protobuf::Message *msg = create_message_by_name(cmd_name);
  if (!msg)
    return;

  auto *descriptor = msg->GetDescriptor();
  auto *reflection = msg->GetReflection();

  for (auto const &[path, value] : args) {
    std::stringstream path_ss(path);
    std::string segment;

    const google::protobuf::Descriptor *current_desc = descriptor;
    google::protobuf::Message *current_msg = msg;

    while (std::getline(path_ss, segment, '.')) {
      const auto *field = current_desc->FindFieldByName(segment);
      if (!field)
        break;

      if (field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
        current_msg = reflection->MutableMessage(current_msg, field);
        current_desc = current_msg->GetDescriptor();
        reflection = current_msg->GetReflection();
      } else {
        set_value_by_type(current_msg, reflection, field, value);
      }
    }
  }

  serialise_to_binary(*msg);
  delete msg;
}

void set_value_by_type(google::protobuf::Message *msg,
                       const google::protobuf::Reflection *reft,
                       const google::protobuf::FieldDescriptor *field,
                       const std::string &value) {
  switch (field->cpp_type()) {
  case google::protobuf::FieldDescriptor::CPPTYPE_INT32:
    reft->SetInt32(msg, field, std::stoi(value));
    break;
  case google::protobuf::FieldDescriptor::CPPTYPE_UINT32:
    reft->SetUInt32(msg, field, std::stoul(value));
    break;
  case google::protobuf::FieldDescriptor::CPPTYPE_BOOL:
    reft->SetBool(msg, field, (value == "true" || value == "1"));
    break;
  case google::protobuf::FieldDescriptor::CPPTYPE_STRING:
    reft->SetString(msg, field, value);
    break;
  case google::protobuf::FieldDescriptor::CPPTYPE_DOUBLE:
    reft->SetDouble(msg, field, std::stod(value));
    break;
  default:
    std::cerr << "Unsupported field type!" << std::endl;
  }
}

void serialise_to_binary(const google::protobuf::Message &msg) {
  std::string binary_data;
  if (msg.SerializeToString(&binary_data)) {
    std::cout << "Successfully serialised " << binary_data.size()
              << " bytes.\n";
    for (unsigned char c : binary_data) {
      printf("%02X ", c);
    }
    std::cout << std::endl;
  }

  std::fstream output("command.bin", std::ios::out | std::ios::binary);
  msg.SerializeToOstream(&output);
}

// ---------------------------------------------------------------------------
// REPL entry point
// ---------------------------------------------------------------------------
void run_cli() {
  char *input;
  const char *prompt = "Marvel> ";

  while ((input = readline(prompt)) != nullptr) {
    if (*input) {
      add_history(input);
      std::string line(input);
      process_command(line);
    }
    free(input);
  }
}

int main() {
  mosquitto_lib_init();
  const char *mqtt_host = std::getenv("MQTT_HOST");
  const char *mqtt_port = std::getenv("MQTT_PORT");
  if (mqtt_host) {
    g_mosq = mosquitto_new("cli_app", true, nullptr);
    if (g_mosq) {
      const int port = mqtt_port ? std::atoi(mqtt_port) : 1883;
      if (mosquitto_connect(g_mosq, mqtt_host, port, 60) != MOSQ_ERR_SUCCESS) {
        std::cerr << "[mqtt] connect to " << mqtt_host << ":" << port << " failed\n";
        mosquitto_destroy(g_mosq);
        g_mosq = nullptr;
      } else {
        std::cout << "[mqtt] connected to " << mqtt_host << ":" << port << '\n';
      }
    }
  }
  init_readline();
  run_cli();
  if (g_mosq) { mosquitto_disconnect(g_mosq); mosquitto_destroy(g_mosq); }
  mosquitto_lib_cleanup();
  return 0;
}

#endif
