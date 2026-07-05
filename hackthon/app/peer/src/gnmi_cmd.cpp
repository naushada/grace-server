#include "gnmi_cmd.hpp"
#include "gnmi_util.hpp"

#include "gnmi/gnmi.pb.h"

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
      m_out("  " + gnmi_util::path_to_string(u.path()) + " = " +
            gnmi_util::typed_value_to_string(u.val()));
}
