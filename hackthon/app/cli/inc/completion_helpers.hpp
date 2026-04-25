#ifndef __completion_helpers_hpp__
#define __completion_helpers_hpp__

#include "lua_engine.hpp"

#include <string>
#include <vector>

// Recursively walk a lua_file::table_type and push dotted-path strings for
// every descendant that matches `filter`.
//
// Output rules:
//   - scalar or array-of-scalars leaf → "<path>="   (ready for a value)
//   - nested table                    → "<path>."   (user can drill deeper)
//
// `prefix` is the dotted path of `table` itself ("" at the root).
// The function is a pure function with no global state — easy to unit-test.
void get_sub_keys(const lua_file::table_type &table,
                  const std::string &prefix,
                  const std::string &filter,
                  std::vector<std::string> &matches);

#endif
