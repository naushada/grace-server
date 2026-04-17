#ifndef __readline_cpp__
#define __readline_cpp__

#include "readline.hpp"

static fs_app fs_mon("/app/command");

////////////////////////////////////////////////////

void run_cli() {
  char *input;
  const char *prompt = "Tarana> "; // <--- SET PROMPT HERE

  while ((input = readline(prompt)) != nullptr) {

    if (*input) {
      add_history(input); // Adds to the up-arrow history

      std::string line(input);
      process_command(line); // Your logic to find the Lua table and Proto
    }

    free(input); // Readline allocates with malloc, you must free it
  }
}

void custom_display_matches(char **matches, int len, int max_len) {
  // 1. Move to a new line before printing matches
  std::cout << "\n";

  // 2. Calculate column width based on max_len
  int column_width = max_len + 2;
  int screen_width = 80; // Ideally get this from ioctl(TIOCGWINSZ)
  int cols = screen_width / column_width;
  if (cols < 1)
    cols = 1;

  // 3. Print matches, but strip the path
  for (int i = 1; matches[i]; i++) {
    std::string full_path(matches[i]);
    size_t last_slash = full_path.find_last_of('/');

    // Only show the part after the last slash
    std::string display_name = (last_slash == std::string::npos)
                                   ? full_path
                                   : full_path.substr(last_slash + 1);

    std::cout << std::left << std::setw(column_width) << display_name;

    if (i % cols == 0)
      std::cout << "\n";
  }

  // 4. Force a redraw of the prompt and current input
  std::cout << "\n";
  rl_on_new_line();
}

void init_readline() {
  /* rl_attempted_completion_over variable to a non-zero value,
     Readline will not perform its default completion even if this function
     returns no matches*/
  rl_attempted_completion_over = 1;

  // 1. Register your custom completion function
  rl_attempted_completion_function = command_completion;

  // 2. Remove '.' and '=' from word breaks
  // This allows Readline to treat "test_param.rate_mbps=50" as one "word"
  // so your generator receives the whole string to parse.
  rl_basic_word_break_characters = " \t\n\"\\'`@$><=;|&{(";

  // 3. Register the display hook here
  rl_completion_display_matches_hook = custom_display_matches;
  // 4. Optional: Enable history (up/down arrows)
  using_history();
}

/* Generator function for command completion.  STATE lets us know whether
 * to start from scratch; without any state (i.e. STATE == 0), then we
 * start at the top of the list.
 * Note: This Function is kept invoking by readline until it returns NULL.
 */
char *command_generator(const char *text, int state) {
  // Use the actual map type directly:
  static std::map<std::string, lua_file::table_type>::const_iterator
      s_members_it_start;
  static std::map<std::string, lua_file::table_type>::const_iterator
      s_members_it_end;

  static std::map<std::string, lua_file::table_type>::const_iterator
      s_command_it;

  std::string command(text);
  std::cout << "Fn:" << __func__ << ":" << __LINE__ << " command:" << command
            << " state:" << state << std::endl;
  /* If this is a new word to complete, initialize now.  This includes
     saving the length of TEXT for efficiency, and initializing the index
     variable to 0. */
  if (!state) {
    auto it = std::find_if(
        fs_mon.lua_engine()->commands().begin(),
        fs_mon.lua_engine()->commands().end(), [&](const auto &ent) {
          auto inner_it = std::find_if(
              ent.second.begin(), ent.second.end(), [&](const auto &element) {
                // Name of the command
                return (!element.first.compare(0, command.length(), command));
              });

          return (inner_it != ent.second.end());
        });

    if (it != fs_mon.lua_engine()->commands().end()) {
      s_command_it = it;
      auto *table_ptr =
          std::get_if<std::shared_ptr<lua_file::table_type>>(&it->second);
      if (table_ptr) {
        s_members_it_start = (*table_ptr)->members.begin();
        s_members_it_end = (*table_ptr)->members.end();
        return (strdup(s_members_it_start->first.c_str()));
      }

      // s_members_it_start = it->second.begin();
      // s_members_it_end = it->second.end();
      // return (strdup(s_members_it_start->first.c_str()));
    }
  }
  /* If no names matched, then return NULL. */
  return ((char *)NULL);
}

char **command_completion(const char *text, std::int32_t start,
                          std::int32_t end) {
  // g_completion_start = start; // Save it here
  //  Prevent default filename completion
  rl_attempted_completion_over = 1;
  std::string what(text);
  if (!start && start == end) {
    // command is entered.
    return rl_completion_matches(text, command_generator);
  } else if (what.empty() && start == end) {
    return rl_completion_matches(text, param_name_generator);
  } else {
    return rl_completion_matches(text, param_value_generator);
  }
}

char *param_value_generator(const char *text, int state) {
  if (!state) {
    // TAB is hit
  }
#if 0
  static size_t list_index;
  static std::vector<std::string> matches;

  // 1. Get the "Command" (the first word on the line)
  std::string param_name(text);

  std::cout << "Fn:" << __func__ << ":" << __LINE__
            << " param_name:" << param_name << " state:" << state << std::endl;
  if (!param_name.empty()) {
    auto &entry = g_current_member_itr->second;
    auto *table_ptr =
        std::get_if<std::shared_ptr<lua_file::table_type>>(&entry);
    if (table_ptr) {
      auto it = (*table_ptr)->members.find(param_name);
      if (it != (*table_ptr)->members.end()) {
      }
    }
    // CASE A: It's a singular value (int, string, etc.)
    if (auto *val = std::get_if<lua_file::value_type>(&entry)) {
      // You've reached a leaf!
      // You can't "dig" deeper. You would likely print or set this value.
      std::cout << "This is a value, not a table.";
    }

    // CASE B: It's a nested table (Message)
    else if (auto *table_ptr =
                 std::get_if<std::shared_ptr<lua_file::table_type>>(&entry)) {
      // This is a branch!
      // You can now access the keys inside (*table_ptr)->members
      for (auto const &[key, _] : (*table_ptr)->members) {
        // These are the "sub-options" for your autocomplete
        matches.push_back(key);
        std::cout << "Fn:" << __func__ << ":" << __LINE__ << " key:" << key
                  << std::endl;
      }
    }

    // CASE C & D: It's a list (Repeated field)
    else if (auto *vec_val =
                 std::get_if<std::vector<lua_file::value_type>>(&entry)) {
      // It's a list of primitive values
    }

  } else {
    // get the value of key now.
  }
#endif
  return nullptr;
}

char *param_name_generator(const char *text, int state) {

  std::string param_name(text);
#if 0
  std::cout << "Fn:" << __func__ << ":" << __LINE__
            << " param_name:" << param_name << " state:" << state << std::endl;
  if (g_current_member_itr == g_current_command_itr->second.members.end()) {
    return (NULL);
  }
  // User hit Tab right after entering the command
  auto &entry = g_current_member_itr->second;

  // CASE A: It's a singular value (int, string, etc.)
  if (auto *val = std::get_if<lua_file::value_type>(&entry)) {
    // You've reached a leaf!
    // You can't "dig" deeper. You would likely print or set this value.
    std::cout << "Fn:" << __func__ << ":" << __LINE__ << " This is a value_type"
              << std::endl;
  }

  // CASE B: It's a nested table (Message)
  else if (auto *table_ptr =
               std::get_if<std::shared_ptr<lua_file::table_type>>(&entry)) {
    if (g_current_member_itr != (*table_ptr)->members.end()) {
      const auto param = (*table_ptr)->members.begin()->first;
      std::cout << "Fn:" << __func__ << ":" << __LINE__
                << " param-name:" << param << std::endl;
      ++g_current_member_itr;
      return (strdup(param.c_str()));
    }
      // This is a branch!
      // You can now access the keys inside (*table_ptr)->members
      for (auto const &[key, _] : (*table_ptr)->members) {
        // These are the "sub-options" for your autocomplete
        matches.push_back(key);
        std::cout << "Fn:" << __func__ << ":" << __LINE__ << " key:" << key
                  << std::endl;
      }
    if (table_ptr) {
      const auto param = (*table_ptr)->members.begin()->first;
      std::cout << "Fn:" << __func__ << ":" << __LINE__
                << " param-name:" << param << std::endl;
      ++g_current_member_itr;
      return (strdup(param.c_str()));
    }
    return (NULL);
  }

  // CASE C & D: It's a list (Repeated field)
  else if (auto *vec_val =
               std::get_if<std::vector<lua_file::value_type>>(&entry)) {
    // It's a list of primitive values
    std::cout << "Fn:" << __func__ << ":" << __LINE__
              << " An array of value_type" << std::endl;
  }

#endif
  // find the matching one
  return nullptr;
}

void get_sub_keys(const lua_file::table_type &table, const std::string &prefix,
                  const std::string &filter,
                  std::vector<std::string> &matches) {
  for (const auto &[key, entry] : table.members) {
    std::string full_path = prefix.empty() ? key : prefix + "." + key;

    // 1. Optimization: Only proceed if this path could match the filter
    if (full_path.compare(0, filter.length(), filter) != 0 &&
        filter.compare(0, full_path.length(), full_path) != 0) {
      continue;
    }

    // 2. If it's a match for what the user typed, add it
    if (full_path.find(filter) == 0) {
      if (std::holds_alternative<lua_file::value_type>(entry)) {
        matches.push_back(full_path + "="); // Leaf value
      } else {
        matches.push_back(full_path + "."); // Nested table
      }
    }

    // 3. Recurse if it's a sub-table
    if (std::holds_alternative<std::shared_ptr<lua_file::table_type>>(entry)) {
      auto sub_ptr = std::get<std::shared_ptr<lua_file::table_type>>(entry);
      if (sub_ptr) {
        // Pass the filter down to keep the recursion efficient
        get_sub_keys(*sub_ptr, full_path, filter, matches);
      }
    }
  }
}

google::protobuf::Message *create_message_by_name(const std::string &cmd_name) {
  // 1. Find the Descriptor for the message by its full name
  // Note: If you use a package in your .proto (e.g., 'package my_app;'),
  // cmd_name must be "my_app.StartDownlinkConnectionsTestRequest"
  const google::protobuf::Descriptor *descriptor =
      google::protobuf::DescriptorPool::generated_pool()->FindMessageTypeByName(
          cmd_name);

  if (!descriptor) {
    std::cerr << "Descriptor not found for message: " << cmd_name << std::endl;
    return nullptr;
  }

  // 2. Use the Generated Message Factory to get the prototype for this
  // descriptor
  const google::protobuf::Message *prototype =
      google::protobuf::MessageFactory::generated_factory()->GetPrototype(
          descriptor);

  if (!prototype) {
    std::cerr << "Prototype not found for descriptor: " << cmd_name
              << std::endl;
    return nullptr;
  }

  // 3. Create a new mutable instance of the message
  return prototype->New();
}

void process_command(const std::string &line) {
  std::stringstream ss(line);
  std::string cmd_name;
  ss >> cmd_name; // First word is the Command (e.g.,

  if (fs_mon.lua_engine()->commands().find(cmd_name) ==
      fs_mon.lua_engine()->commands().end()) {
    std::cout << "Unknown command: " << cmd_name << std::endl;
    return;
  }

  // Parse the remaining parts into key=value pairs
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
  // 1. Create the specific Protobuf message instance (e.g., using a
  // Factory) For this example, let's assume 'msg' is your
  // StartDownlinkConnectionsTestRequest object
  // Create the Protobuf message dynamically
  google::protobuf::Message *msg = create_message_by_name(cmd_name);

  if (msg) {
    // Parse arguments and apply them via reflection (as discussed
    // previously)
    // ... apply_to_proto(msg, arguments) ...
    // Now, apply these arguments to your Protobuf message

    auto *descriptor = msg->GetDescriptor();
    auto *reflection = msg->GetReflection();

    for (auto const &[path, value] : args) {
      // 2. Handle nested paths (e.g., "test_param.rate_mbps")
      std::stringstream path_ss(path);
      std::string segment;

      const google::protobuf::Descriptor *current_desc = descriptor;
      google::protobuf::Message *current_msg = msg;

      // Navigate to the leaf message
      while (std::getline(path_ss, segment, '.')) {
        const auto *field = current_desc->FindFieldByName(segment);
        if (!field)
          break;

        if (field->type() == google::protobuf::FieldDescriptor::TYPE_MESSAGE) {
          // Move deeper into the nested message
          current_msg = reflection->MutableMessage(current_msg, field);
          current_desc = current_msg->GetDescriptor();
          reflection = current_msg->GetReflection();
        } else {
          // 3. We reached a Leaf Field (int, string, bool)
          set_value_by_type(current_msg, reflection, field, value);
        }
      }
    }
    serialise_to_binary(*msg); // For the actual network transmission
    // Don't forget to delete the message when finished!
    delete msg;
  }
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
  // 1. Serialise to string (bytes)
  std::string binary_data;
  if (msg.SerializeToString(&binary_data)) {
    std::cout << "Successfully serialised " << binary_data.size()
              << " bytes.\n";

    // Optional: Hex dump for debugging
    for (unsigned char c : binary_data) {
      printf("%02X ", c);
    }
    std::cout << std::endl;
  }

  // 2. Or serialise directly to a file
  std::fstream output("command.bin", std::ios::out | std::ios::binary);
  msg.SerializeToOstream(&output);
}

int main() {
  init_readline();
  run_cli();
  return (0);
}

#endif
