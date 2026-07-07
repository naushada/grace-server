#ifndef __update_sink_hpp__
#define __update_sink_hpp__

// update_sink decouples the local gNMI Set handler (client_app.cpp) from any
// UI that wants to observe incoming operations. The gnmi_peer TUI registers a
// callback; the Set handler forwards each received update/replace/delete as a
// preformatted line. When no callback is registered (the `app` and `cli_app`
// binaries) emit() is a no-op, so the handler's behaviour is unchanged.
//
// Single-threaded: the Set handler and any registrant both run on the shared
// libevent loop thread, so no locking is required.

#include <functional>
#include <string>
#include <utility>
#include <vector>

class update_sink {
public:
  using cb_t = std::function<void(const std::string &)>;

  static update_sink &instance() {
    static update_sink s;
    return s;
  }

  // Append a subscriber (multiple may coexist — e.g. a TUI and a file logger).
  void add(cb_t cb) {
    if (cb)
      m_cbs.push_back(std::move(cb));
  }
  // Replace all subscribers with one (back-compat for single-observer UIs).
  void set(cb_t cb) {
    m_cbs.clear();
    add(std::move(cb));
  }
  void clear() { m_cbs.clear(); }

  void emit(const std::string &line) {
    for (const auto &cb : m_cbs)
      if (cb)
        cb(line);
  }

private:
  std::vector<cb_t> m_cbs;
};

#endif // __update_sink_hpp__
