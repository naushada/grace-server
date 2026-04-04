#ifndef __fs_app_hpp__
#define __fs_app_hpp__

#include "framework.hpp"

#include "lua_engine.hpp"
#include <filesystem>
#include <iostream>
#include <lua.hpp>
#include <vector>

extern "C" {
#include <cstring>
#include <linux/limits.h>
#include <sys/inotify.h>
#include <unistd.h>
}

class fs_app : public evt_io {

public:
  using handle_t = std::int32_t;
  fs_app(const std::string &location)
      : evt_io(m_channel = inotify_init()),
        m_watch_channel(inotify_add_watch(
            m_channel, location.c_str(),
            (IN_CREATE | IN_MODIFY | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM))),
        m_old_event(), m_lua_engine(std::make_unique<lua_file>()) {
    if (m_watch_channel < 0) {
      std::cout << "Fn:" << __func__ << ":" << __LINE__
                << " inotify_add_watch failed for channel:" << m_channel
                << std::endl;
    } else {
      // read all file on boot & latter on modification,delete,moved.
      on_boot(location);
      std::cout << "Fn:" << __func__ << ":" << __LINE__
                << " channel:" << m_channel
                << " watch_channel:" << m_watch_channel << std::endl;
    }
  }

  virtual ~fs_app() {
    if (m_watch_channel > 0) {
      inotify_rm_watch(m_channel, m_watch_channel);
      std::cout << "Fn:" << __func__ << ":" << __LINE__
                << " stopped folder monitoring for channel:" << m_channel
                << std::endl;
    }
    close(m_channel);
  }

  std::int32_t on_boot(const std::string &folder_path);
  std::int32_t process_inotify_onchange(const std::string &in);
  virtual std::int32_t handle_read(const std::int32_t &channel,
                                   const std::string &data,
                                   const bool &dry_run) override;
  virtual std::int32_t handle_event(const std::int32_t &channel,
                                    const std::uint16_t &event) override;
  virtual std::int32_t handle_write(const std::int32_t &channel) override;
  virtual std::int32_t handle_close(const std::int32_t &channel) override;

private:
  handle_t m_channel;
  handle_t m_watch_channel;
  std::vector<std::uint8_t> m_old_event;
  std::unique_ptr<lua_file> m_lua_engine;
};

#endif
