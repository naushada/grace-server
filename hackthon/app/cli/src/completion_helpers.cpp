#include "completion_helpers.hpp"

#include <variant>

void get_sub_keys(const lua_file::table_type &table, const std::string &prefix,
                  const std::string &filter,
                  std::vector<std::string> &matches) {
  for (const auto &[key, entry] : table.members) {
    std::string full_path = prefix.empty() ? key : prefix + "." + key;

    // Could this path (or some descendant of it) still match the filter?
    //   forward  — the path already starts with the filter
    //   backward — the filter starts with the path (so a deeper descendant
    //              may still match)
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
