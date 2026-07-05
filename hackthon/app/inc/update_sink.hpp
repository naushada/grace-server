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

class update_sink {
public:
  using cb_t = std::function<void(const std::string &)>;

  static update_sink &instance() {
    static update_sink s;
    return s;
  }

  void set(cb_t cb) { m_cb = std::move(cb); }
  void clear() { m_cb = nullptr; }

  void emit(const std::string &line) {
    if (m_cb)
      m_cb(line);
  }

private:
  cb_t m_cb;
};

#endif // __update_sink_hpp__
