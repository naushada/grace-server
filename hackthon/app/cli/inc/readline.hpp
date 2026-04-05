#ifndef __readline_hpp__
#define __readline_hpp__

#include "fs_app.hpp"
#include "lua_engine.hpp"

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>

#include <iomanip>
#include <readline/history.h>
#include <readline/readline.h>

google::protobuf::Message *create_message_by_name(const std::string &cmd_name);
void apply_to_proto(const std::string &cmd_name,
                    const std::map<std::string, std::string> &args);

void process_command(const std::string &line);

void get_sub_keys(const lua_file::table_type &table, const std::string &prefix,
                  const std::string &filter, std::vector<std::string> &matches);

char *lua_option_generator(const char *text, int state);

char **lua_command_completion(const char *text, std::int32_t start,
                              std::int32_t end);

void set_value_by_type(google::protobuf::Message *msg,
                       const google::protobuf::Reflection *reft,
                       const google::protobuf::FieldDescriptor *field,
                       const std::string &value);

void init_readline();
void run_cli();
void custom_display_matches(char **matches, int len, int max_len);
void serialise_to_binary(const google::protobuf::Message &msg);

#endif
