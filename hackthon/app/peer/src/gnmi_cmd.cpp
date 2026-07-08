// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Naushad

#include "gnmi_cmd.hpp"
#include "gnmi_util.hpp"

#include "gnmi/gnmi.pb.h"

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <sstream>
#include <utility>

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------

static std::string trim(const std::string &s) {
  const auto b = s.find_first_not_of(" \t");
  if (b == std::string::npos)
    return "";
  const auto e = s.find_last_not_of(" \t");
  return s.substr(b, e - b + 1);
}

// Parse a duration like "10s", "500ms", "250us", "1000ns", "1m", "2h", or a
// bare number (seconds) into nanoseconds. Returns 0 on parse failure.
static std::uint64_t parse_duration_ns(const std::string &s) {
  std::size_t i = 0;
  while (i < s.size() && (std::isdigit((unsigned char)s[i]) || s[i] == '.'))
    ++i;
  if (i == 0)
    return 0;
  const double val = std::strtod(s.substr(0, i).c_str(), nullptr);
  const std::string unit = s.substr(i);
  double mult;
  if (unit.empty() || unit == "s")
    mult = 1e9;
  else if (unit == "ms")
    mult = 1e6;
  else if (unit == "us")
    mult = 1e3;
  else if (unit == "ns")
    mult = 1.0;
  else if (unit == "m")
    mult = 60e9;
  else if (unit == "h")
    mult = 3600e9;
  else
    return 0;
  return static_cast<std::uint64_t>(val * mult);
}

static const char *op_name(gnmi::UpdateResult::Operation op) {
  switch (op) {
  case gnmi::UpdateResult::UPDATE:
    return "UPDATE";
  case gnmi::UpdateResult::REPLACE:
    return "REPLACE";
  case gnmi::UpdateResult::DELETE:
    return "DELETE";
  default:
    return "OP";
  }
}

// ---------------------------------------------------------------------------
// gnmi_cmd
// ---------------------------------------------------------------------------

gnmi_cmd::gnmi_cmd(endpoint remote, tls_config tls, out_fn out)
    : m_remote(std::move(remote)), m_tls(std::move(tls)),
      m_out(std::move(out)) {}

bool gnmi_cmd::dispatch(const std::string &line) {
  std::istringstream ss(line);
  std::string verb;
  ss >> verb;
  if (verb == "gnmi")
    ss >> verb; // allow "gnmi set" and bare "set"

  std::string spec;
  std::getline(ss, spec);
  spec = trim(spec);

  if (verb == "set") {
    do_set(spec);
  } else if (verb == "get") {
    do_get(spec);
  } else if (verb == "subscribe" || verb == "sub") {
    do_subscribe(spec);
  } else if (verb == "help") {
    help();
  } else if (verb == "quit" || verb == "exit") {
    return false;
  } else if (!verb.empty()) {
    m_out("unknown command: '" + verb + "' (try 'help')");
  }
  return true;
}

void gnmi_cmd::help() {
  m_out("gnmi_peer commands:");
  m_out("  gnmi set <xpath>:<value>[,<xpath>:<value>...]  send SetRequest "
        "(role ADMIN)");
  m_out("  gnmi get <xpath>[,<xpath>...]                  send GetRequest "
        "(role VIEWER)");
  m_out("  gnmi subscribe <xpath>[,<xpath>...] [interval]  stream telemetry; "
        "interval (10s/500ms/1m) => SAMPLE, omit => on-change");
  m_out("  help                                           show this help");
  m_out("  quit | exit                                    leave gnmi_peer");
  m_out("notes: xpath uses '/'-separated YANG form, e.g. "
        "/interfaces/interface[name=eth0]/config/mtu");
  m_out("       the value is everything after the FIRST ':' in each pair");
}

void gnmi_cmd::do_set(const std::string &spec) {
  if (spec.empty()) {
    m_out("usage: gnmi set <xpath>:<value>[,<xpath>:<value>...]");
    return;
  }

  gnmi::SetRequest req;
  // The server enforces RBAC: Set requires prefix.target == "ADMIN".
  req.mutable_prefix()->set_target("ADMIN");

  int n = 0;
  std::istringstream ss(spec);
  std::string pair;
  while (std::getline(ss, pair, ',')) {
    pair = trim(pair);
    if (pair.empty())
      continue;
    const auto colon = pair.find(':'); // split on the FIRST ':'
    if (colon == std::string::npos) {
      m_out("  skip (no ':' in pair): " + pair);
      continue;
    }
    const std::string xpath = trim(pair.substr(0, colon));
    const std::string value = pair.substr(colon + 1);
    auto *upd = req.add_update();
    *upd->mutable_path() = gnmi_util::parse_yang_path(xpath);
    gnmi_util::set_typed_value(upd->mutable_val(), value);
    ++n;
  }

  if (n == 0) {
    m_out("no valid <xpath>:<value> pairs");
    return;
  }

  std::string pb;
  req.SerializeToString(&pb);
  m_out("[set] -> " + m_remote.host + ":" + std::to_string(m_remote.port) +
        " (" + std::to_string(n) + " update(s))");

  gnmi_client::push_async(
      m_remote.host, m_remote.port, "/gnmi.gNMI/Set", pb, m_tls,
      [this](const gnmi_client::response &r) { render_set_resp(r); });
}

void gnmi_cmd::do_get(const std::string &spec) {
  if (spec.empty()) {
    m_out("usage: gnmi get <xpath>[,<xpath>...]");
    return;
  }

  gnmi::GetRequest req;
  req.mutable_prefix()->set_target("VIEWER");

  int n = 0;
  std::istringstream ss(spec);
  std::string path;
  while (std::getline(ss, path, ',')) {
    path = trim(path);
    if (path.empty())
      continue;
    *req.add_path() = gnmi_util::parse_yang_path(path);
    ++n;
  }

  if (n == 0) {
    m_out("no valid xpaths");
    return;
  }

  req.set_encoding(gnmi::JSON);
  std::string pb;
  req.SerializeToString(&pb);
  m_out("[get] -> " + m_remote.host + ":" + std::to_string(m_remote.port) +
        " (" + std::to_string(n) + " path(s))");

  gnmi_client::push_async(
      m_remote.host, m_remote.port, "/gnmi.gNMI/Get", pb, m_tls,
      [this](const gnmi_client::response &r) { render_get_resp(r); });
}

void gnmi_cmd::do_subscribe(const std::string &spec) {
  if (spec.empty()) {
    m_out("usage: gnmi subscribe <xpath>[,<xpath>...] [interval]   "
          "(interval e.g. 10s, 500ms, 1m => SAMPLE; omit => on-change)");
    return;
  }

  // Peel an optional trailing interval token. xpaths start with '/', so a
  // trailing whitespace-separated token that starts with a digit is the
  // interval. With one, each subscription is SAMPLE mode at that interval;
  // without one, TARGET_DEFINED (the device decides — typically the current
  // state, then on-change).
  std::string paths = spec;
  std::uint64_t interval_ns = 0;
  std::string interval_disp;
  if (const std::size_t ws = spec.find_last_of(" \t");
      ws != std::string::npos) {
    const std::string tail = trim(spec.substr(ws + 1));
    if (!tail.empty() && std::isdigit((unsigned char)tail[0])) {
      interval_ns = parse_duration_ns(tail);
      if (interval_ns == 0) {
        m_out("bad interval '" + tail + "' (e.g. 10s, 500ms, 1m)");
        return;
      }
      interval_disp = tail;
      paths = spec.substr(0, ws);
    }
  }

  gnmi::SubscribeRequest req;
  auto *sl = req.mutable_subscribe();
  sl->set_mode(gnmi::SubscriptionList::STREAM);
  sl->mutable_prefix()->set_target("VIEWER");
  sl->set_encoding(gnmi::JSON);

  int n = 0;
  std::istringstream ss(paths);
  std::string path;
  while (std::getline(ss, path, ',')) {
    path = trim(path);
    if (path.empty())
      continue;
    auto *sub = sl->add_subscription();
    *sub->mutable_path() = gnmi_util::parse_yang_path(path);
    if (interval_ns > 0) {
      sub->set_mode(gnmi::SAMPLE);
      sub->set_sample_interval(interval_ns);
    }
    ++n;
  }
  if (n == 0) {
    m_out("no valid xpaths");
    return;
  }

  std::string pb;
  req.SerializeToString(&pb);
  m_out("[sub] -> " + m_remote.host + ":" + std::to_string(m_remote.port) +
        " (" + std::to_string(n) + " path(s), STREAM" +
        (interval_ns ? ", SAMPLE @" + interval_disp : ", on-change") + ")");

  gnmi_client::subscribe_async(
      m_remote.host, m_remote.port, pb, m_tls,
      [this](const std::string &resp_pb) {
        gnmi::SubscribeResponse r;
        if (r.ParseFromString(resp_pb))
          m_out("[sub] " + gnmi_util::subscribe_response_to_json(r));
        else
          m_out("[sub] (unparsable response)");
      },
      [this](const gnmi_client::response &r) {
        if (r.grpc_status > 0)
          m_out("[sub] stream ended, status=" + std::to_string(r.grpc_status) +
                (r.grpc_message.empty() ? "" : " msg=" + r.grpc_message));
        else
          m_out("[sub] stream ended");
      });
}

void gnmi_cmd::render_set_resp(const gnmi_client::response &r) {
  if (r.grpc_status < 0) {
    m_out("[set] transport error: " + r.grpc_message);
    return;
  }
  if (r.grpc_status != 0) {
    m_out("[set] error status=" + std::to_string(r.grpc_status) +
          (r.grpc_message.empty() ? "" : " msg=" + r.grpc_message));
    return;
  }
  gnmi::SetResponse resp;
  if (!resp.ParseFromString(r.body_pb)) {
    m_out("[set] OK (response parse failed)");
    return;
  }
  m_out("[set] OK, " + std::to_string(resp.response_size()) + " result(s)");
  for (const auto &ur : resp.response())
    m_out(std::string("  ") + op_name(ur.op()) + " " +
          gnmi_util::path_to_string(ur.path()));
}

void gnmi_cmd::render_get_resp(const gnmi_client::response &r) {
  if (r.grpc_status < 0) {
    m_out("[get] transport error: " + r.grpc_message);
    return;
  }
  if (r.grpc_status != 0) {
    m_out("[get] error status=" + std::to_string(r.grpc_status) +
          (r.grpc_message.empty() ? "" : " msg=" + r.grpc_message));
    return;
  }
  gnmi::GetResponse resp;
  if (!resp.ParseFromString(r.body_pb)) {
    m_out("[get] OK (response parse failed)");
    return;
  }
  m_out("[get] OK, " + std::to_string(resp.notification_size()) +
        " notification(s)");
  for (const auto &notif : resp.notification())
    for (const auto &u : notif.update())
      m_out("  " + gnmi_util::update_to_json(u));
}
