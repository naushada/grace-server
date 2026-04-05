#ifndef __readline_cpp__
#define __readline_cpp__

#include "readline.hpp"

static fs_app fs_mon("/app/command");
int g_completion_start = 0;

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

void init_readline() {
  // 1. Register your custom completion function
  rl_attempted_completion_function = lua_command_completion;

  // 2. Remove '.' and '=' from word breaks
  // This allows Readline to treat "test_param.rate_mbps=50" as one "word"
  // so your generator receives the whole string to parse.
  rl_basic_word_break_characters = " \t\n\"\\'`@$><=;|&{(";

  // 3. Optional: Enable history (up/down arrows)
  using_history();
}

char **lua_command_completion(const char *text, std::int32_t start,
                              std::int32_t end) {
  g_completion_start = start; // Save it here
  // Prevent default filename completion
  rl_attempted_completion_over = 1;

  // rl_line_buffer contains the full line. We use it to determine context.
  return rl_completion_matches(text, lua_option_generator);
}

char *lua_option_generator(const char *text, int state) {
  static size_t list_index;
  static std::vector<std::string> matches;

  if (!state) {
    list_index = 0;
    matches.clear();

    // 1. Get the "Command" (the first word on the line)
    std::string filter(text);

    // 2. If we are typing the first word, suggest top-level Commands
    if (g_completion_start == 0) {
      for (auto const &[name, _] : fs_mon.lua_engine()->commands()) {
        if (name.find(filter) == 0)
          matches.push_back(name);
      }
    } else {
      // 1. Find which command was typed first on the line
      std::stringstream ss(rl_line_buffer);
      std::string command;
      ss >> command;
      if (fs_mon.lua_engine()->commands().count(command)) {
        // Helper function to crawl your table_type and find children
        // of whatever the user has partially typed so far
        get_sub_keys(fs_mon.lua_engine()->commands().at(command), "", filter,
                     matches);
      }
    }
  }

  if (list_index < matches.size()) {
    return strdup(matches[list_index++].c_str());
  }

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
  // 1. Create the specific Protobuf message instance (e.g., using a Factory)
  // For this example, let's assume 'msg' is your
  // StartDownlinkConnectionsTestRequest object
  // Create the Protobuf message dynamically
  google::protobuf::Message *msg = create_message_by_name(cmd_name);

  if (msg) {
    // Parse arguments and apply them via reflection (as discussed previously)
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

int main() {
  init_readline();
  run_cli();
  return (0);
}

#endif
