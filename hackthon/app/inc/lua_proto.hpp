#ifndef __lua_proto_hpp__
#define __lua_proto_hpp__

// Populate a protobuf message from a lua_file table via reflection, so a request
// can be described declaratively in a .lua file and serialized straight into its
// proto — no per-type C++ builder.
//
// Mapping (lua_file::table_type -> Message):
//   scalar               -> singular scalar field (int/uint/double/bool/string)
//   string (enum field)  -> enum value by name (or number)
//   array of scalars     -> repeated scalar field
//   nested table         -> nested message field (recurse)
//   array of tables      -> repeated message field
//   nested table + @type -> google.protobuf.Any (build @type, populate, PackFrom)
//   string (gnmi.Path)   -> parsed as a YANG path (sugar; also array->repeated Path)
//
// The @type value is a fully-qualified proto name, e.g. "gnmi.GetRequest" or
// "tnmi.DeviceRequest.CliRequest"; it must be linked into the binary.

#include "lua_engine.hpp"

#include <google/protobuf/message.h>
#include <string>

namespace lua_proto {

// Empty message of a fully-qualified proto name from the generated pool, or
// nullptr (+ err). Caller owns the returned message.
google::protobuf::Message *create_by_name(const std::string &name,
                                          std::string &err);

// Populate `msg` from `tbl`. Returns true on success; false + err on the first
// problem (unknown field, type mismatch, unknown @type/enum, …).
bool populate(const lua_file::table_type &tbl, google::protobuf::Message *msg,
              std::string &err);

} // namespace lua_proto

#endif // __lua_proto_hpp__
