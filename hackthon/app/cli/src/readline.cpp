#ifndef __readline_cpp__
#define __readline_cpp__

#include "readline.hpp"

#include <algorithm>
#include <cstring>

// ---------------------------------------------------------------------------
// Single filesystem monitor — owns the lua_file engine that the CLI queries.
// ---------------------------------------------------------------------------
static fs_app fs_mon("/app/command");

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
// Recursive dotted-path walker used by param_name_generator.
// ---------------------------------------------------------------------------
void get_sub_keys(const lua_file::table_type &table, const std::string &prefix,
                  const std::string &filter,
                  std::vector<std::string> &matches) {
  for (const auto &[key, entry] : table.members) {
    std::string full_path = prefix.empty() ? key : prefix + "." + key;

    // Could this path (or some descendant of it) still match the filter?
    bool forward = full_path.compare(0, filter.length(), filter) == 0;
    bool backward = filter.compare(0, full_path.length(), full_path) == 0;
    if (!forward && !backward)
      continue;

    if (forward) {
      if (std::holds_alternative<lua_file::value_type>(entry) ||
          std::holds_alternative<std::vector<lua_file::value_type>>(entry)) {
        matches.push_back(full_path + "=");
      } else if (std::holds_alternative<std::shared_ptr<lua_file::table_type>>(
                     entry)) {
        matches.push_back(full_path + ".");
      }
    }

    if (auto sub_ptr =
            std::get_if<std::shared_ptr<lua_file::table_type>>(&entry)) {
      if (*sub_ptr) {
        get_sub_keys(**sub_ptr, full_path, filter, matches);
      }
    }
  }
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

  std::map<std::string, std::string> arguments;
  std::string pair;
  while (ss >> pair) {
    size_t pos = pair.find('=');
    if (pos != std::string::npos) {
      std::string key = pair.substr(0, pos);
      std::string val = pair.substr(pos + 1);
      arguments[key] = val;
    }
  }

  apply_to_proto(cmd_name, arguments);
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
  const char *prompt = "Tarana> ";

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
  init_readline();
  run_cli();
  return 0;
}

#endif
