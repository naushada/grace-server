// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#include "lua_proto.hpp"
#include "gnmi_util.hpp"

#include "gnmi/gnmi.pb.h"

#include <google/protobuf/any.pb.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/message.h>

#include <memory>
#include <string>
#include <variant>
#include <vector>

using google::protobuf::Descriptor;
using google::protobuf::DescriptorPool;
using google::protobuf::EnumValueDescriptor;
using google::protobuf::FieldDescriptor;
using google::protobuf::Message;
using google::protobuf::MessageFactory;
using google::protobuf::Reflection;

using table_type = lua_file::table_type;
using entry_type = lua_file::entry_type;
using value_type = lua_file::value_type;

// ---------------------------------------------------------------------------
// value_type extractors (value_type derives from std::variant)
// ---------------------------------------------------------------------------
static bool as_str(const value_type &v, std::string &o) {
  if (std::holds_alternative<std::string>(v)) { o = std::get<std::string>(v); return true; }
  return false;
}
static bool as_ll(const value_type &v, long long &o) {
  if (std::holds_alternative<std::int32_t>(v)) { o = std::get<std::int32_t>(v); return true; }
  if (std::holds_alternative<std::uint32_t>(v)) { o = std::get<std::uint32_t>(v); return true; }
  if (std::holds_alternative<double>(v)) { o = static_cast<long long>(std::get<double>(v)); return true; }
  if (std::holds_alternative<bool>(v)) { o = std::get<bool>(v) ? 1 : 0; return true; }
  return false;
}
static bool as_dbl(const value_type &v, double &o) {
  if (std::holds_alternative<double>(v)) { o = std::get<double>(v); return true; }
  long long n; if (as_ll(v, n)) { o = static_cast<double>(n); return true; }
  return false;
}
static bool as_bl(const value_type &v, bool &o) {
  if (std::holds_alternative<bool>(v)) { o = std::get<bool>(v); return true; }
  long long n; if (as_ll(v, n)) { o = (n != 0); return true; }
  return false;
}

static bool terr(std::string &err, const FieldDescriptor *f, const char *want) {
  err = "field '" + f->name() + "' expects " + want;
  return false;
}

static bool is_path(const FieldDescriptor *f) {
  return f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE &&
         f->message_type()->full_name() == "gnmi.Path";
}

// Set (rep=false) or Add (rep=true) a scalar/enum field from a lua value.
static bool put_scalar(Message *m, const FieldDescriptor *f, const value_type &v,
                       const Reflection *r, bool rep, std::string &err) {
  using FD = FieldDescriptor;
  switch (f->cpp_type()) {
  case FD::CPPTYPE_INT32: { long long n; if (!as_ll(v, n)) return terr(err, f, "int"); rep ? r->AddInt32(m, f, (int32_t)n) : r->SetInt32(m, f, (int32_t)n); break; }
  case FD::CPPTYPE_INT64: { long long n; if (!as_ll(v, n)) return terr(err, f, "int"); rep ? r->AddInt64(m, f, n) : r->SetInt64(m, f, n); break; }
  case FD::CPPTYPE_UINT32: { long long n; if (!as_ll(v, n)) return terr(err, f, "int"); rep ? r->AddUInt32(m, f, (uint32_t)n) : r->SetUInt32(m, f, (uint32_t)n); break; }
  case FD::CPPTYPE_UINT64: { long long n; if (!as_ll(v, n)) return terr(err, f, "int"); rep ? r->AddUInt64(m, f, (uint64_t)n) : r->SetUInt64(m, f, (uint64_t)n); break; }
  case FD::CPPTYPE_DOUBLE: { double d; if (!as_dbl(v, d)) return terr(err, f, "number"); rep ? r->AddDouble(m, f, d) : r->SetDouble(m, f, d); break; }
  case FD::CPPTYPE_FLOAT: { double d; if (!as_dbl(v, d)) return terr(err, f, "number"); rep ? r->AddFloat(m, f, (float)d) : r->SetFloat(m, f, (float)d); break; }
  case FD::CPPTYPE_BOOL: { bool b; if (!as_bl(v, b)) return terr(err, f, "bool"); rep ? r->AddBool(m, f, b) : r->SetBool(m, f, b); break; }
  case FD::CPPTYPE_STRING: { std::string s; if (!as_str(v, s)) return terr(err, f, "string"); rep ? r->AddString(m, f, s) : r->SetString(m, f, s); break; }
  case FD::CPPTYPE_ENUM: {
    const EnumValueDescriptor *ev = nullptr;
    std::string s;
    if (as_str(v, s)) {
      ev = f->enum_type()->FindValueByName(s);
      if (!ev) { err = "enum '" + f->name() + "' has no value '" + s + "'"; return false; }
    } else {
      long long n; if (!as_ll(v, n)) return terr(err, f, "enum name/number");
      ev = f->enum_type()->FindValueByNumber((int)n);
      if (!ev) { err = "enum '" + f->name() + "' has no number " + std::to_string(n); return false; }
    }
    rep ? r->AddEnum(m, f, ev) : r->SetEnum(m, f, ev);
    break;
  }
  default: err = "unsupported field type for '" + f->name() + "'"; return false;
  }
  return true;
}

static bool pack_any(const table_type &sub, Message *any_msg, std::string &err);

// ---------------------------------------------------------------------------
Message *lua_proto::create_by_name(const std::string &name, std::string &err) {
  const Descriptor *d =
      DescriptorPool::generated_pool()->FindMessageTypeByName(name);
  if (!d) { err = "unknown proto type '" + name + "'"; return nullptr; }
  const Message *proto = MessageFactory::generated_factory()->GetPrototype(d);
  if (!proto) { err = "no prototype for '" + name + "'"; return nullptr; }
  return proto->New();
}

bool lua_proto::populate(const table_type &tbl, Message *msg, std::string &err) {
  const Descriptor *d = msg->GetDescriptor();
  const Reflection *r = msg->GetReflection();
  for (const auto &kv : tbl.members) {
    const std::string &name = kv.first;
    if (name == "@type") continue; // consumed by the enclosing Any
    const FieldDescriptor *f = d->FindFieldByName(name);
    if (!f) { err = std::string(d->full_name()) + " has no field '" + name + "'"; return false; }
    const entry_type &e = kv.second;

    if (const value_type *v = std::get_if<value_type>(&e)) {
      if (f->is_repeated()) { err = "field '" + name + "' is repeated — provide an array"; return false; }
      if (f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
        std::string s;
        if (is_path(f) && as_str(*v, s)) {
          *static_cast<gnmi::Path *>(r->MutableMessage(msg, f)) = gnmi_util::parse_yang_path(s);
          continue;
        }
        err = "field '" + name + "' is a message — provide a table"; return false;
      }
      if (!put_scalar(msg, f, *v, r, false, err)) return false;
    } else if (const auto *arr = std::get_if<std::vector<value_type>>(&e)) {
      if (!f->is_repeated()) { err = "field '" + name + "' is not repeated — provide a scalar"; return false; }
      if (f->cpp_type() == FieldDescriptor::CPPTYPE_MESSAGE) {
        if (is_path(f)) {
          for (const auto &ev : *arr) {
            std::string s;
            if (as_str(ev, s)) *static_cast<gnmi::Path *>(r->AddMessage(msg, f)) = gnmi_util::parse_yang_path(s);
          }
          continue;
        }
        err = "repeated message '" + name + "' — provide an array of tables"; return false;
      }
      for (const auto &ev : *arr) if (!put_scalar(msg, f, ev, r, true, err)) return false;
    } else if (const auto *sub = std::get_if<std::shared_ptr<table_type>>(&e)) {
      // Map field (e.g. gnmi PathElem.key = map<string,string>): a nested table
      // of key -> value; each pair becomes a map entry.
      if (f->is_map()) {
        const Descriptor *ed = f->message_type();
        const FieldDescriptor *kf = ed->FindFieldByName("key");
        const FieldDescriptor *vf = ed->FindFieldByName("value");
        if (!kf || !vf) { err = "field '" + name + "' is not a well-formed map"; return false; }
        for (const auto &mkv : (*sub)->members) {
          Message *entry = r->AddMessage(msg, f);
          const Reflection *er = entry->GetReflection();
          er->SetString(entry, kf, mkv.first);
          const value_type *mv = std::get_if<value_type>(&mkv.second);
          if (!mv || !put_scalar(entry, vf, *mv, er, false, err)) {
            if (err.empty()) err = "map '" + name + "' value must be a scalar";
            return false;
          }
        }
        continue;
      }
      if (f->is_repeated()) { err = "field '" + name + "' is repeated — provide an array"; return false; }
      if (f->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) { err = "field '" + name + "' is not a message"; return false; }
      Message *sm = r->MutableMessage(msg, f);
      if (f->message_type()->full_name() == "google.protobuf.Any") {
        if (!pack_any(**sub, sm, err)) return false;
      } else if (!populate(**sub, sm, err)) {
        return false;
      }
    } else if (const auto *sarr = std::get_if<std::vector<std::shared_ptr<table_type>>>(&e)) {
      if (!f->is_repeated() || f->cpp_type() != FieldDescriptor::CPPTYPE_MESSAGE) {
        err = "field '" + name + "' — expected a repeated message"; return false;
      }
      for (const auto &st : *sarr) {
        Message *em = r->AddMessage(msg, f);
        if (!populate(*st, em, err)) return false;
      }
    }
  }
  return true;
}

// Build the @type message, populate it, and PackFrom into the Any.
static bool pack_any(const table_type &sub, Message *any_msg, std::string &err) {
  auto it = sub.members.find("@type");
  if (it == sub.members.end()) { err = "Any field needs an '@type' key"; return false; }
  const value_type *tv = std::get_if<value_type>(&it->second);
  std::string type;
  if (!tv || !as_str(*tv, type)) { err = "'@type' must be a string"; return false; }
  std::unique_ptr<Message> inner(lua_proto::create_by_name(type, err));
  if (!inner) return false;
  if (!lua_proto::populate(sub, inner.get(), err)) return false;
  static_cast<google::protobuf::Any *>(any_msg)->PackFrom(*inner);
  return true;
}
