#ifndef __fs_app_cpp__
#define __fs_app_cpp__

#include "fs_app.hpp"

std::int32_t fs_app::process_inotify_onchange(const std::string &in) {

  auto offset = 0;
  do {
    struct inotify_event *event = (struct inotify_event *)(in.data() + offset);
    if (event->len) {
      // This is a file
      namespace fs = std::filesystem;
      std::string fname(event->name);
      fs::path fn(fname);
      // fn.extension() will return extention with double quotes within, hence
      // using string to get rid of this.
      if (fn.extension().string() == std::string(".lua")) {
        if (event->mask & IN_CREATE) {
          // lua_engine().update_command(fname);
          std::cout << "Fn:" << __func__ << ":" << __LINE__
                    << " This file:" << fname << " is created" << std::endl;
        } else if (event->mask & IN_MODIFY) {
          // lua_engine().update_command(fname);
          std::cout << "Fn:" << __func__ << ":" << __LINE__
                    << " This file:" << fname << " is modifed" << std::endl;
        } else if (event->mask & IN_MOVED_TO) {
          // lua_engine().update_command(fname);
          if (!m_old_event.empty()) {
            auto *old_event =
                reinterpret_cast<struct inotify_event *>(m_old_event.data());
            if (old_event->cookie == event->cookie) {
              std::cout << "Fn:" << __func__ << ":" << __LINE__
                        << " old-file-name:" << old_event->name
                        << " new-file-name:" << event->name << std::endl;
              // clear old event now
              m_old_event.clear();
            }
          } else {
            // file is moved from non-watch location to watch location
            // treat this a new file.
            std::cout << "Fn:" << __func__ << ":" << __LINE__
                      << " This file:" << fname
                      << " is moved to watched location" << std::endl;
          }
        } else if (event->mask & IN_MOVED_FROM) {
          // Insert into MAP - key=filename
          // lua_engine().update_command(fname);
          std::cout << "Fn:" << __func__ << ":" << __LINE__
                    << " This file:" << fname << " old file name" << std::endl;
          m_old_event.resize(sizeof(struct inotify_event) + event->len);
          std::memcpy(m_old_event.data(), event, m_old_event.size());
        } else if (event->mask & IN_DELETE) {
          // Remove entry from MAP
          // lua_engine().delete_command(fname);
          std::cout << "Fn:" << __func__ << ":" << __LINE__
                    << " This file:" << fname << " is deleted" << std::endl;
        } else {
          std::cerr << "Fn:" << __func__ << ":" << __LINE__
                    << " mask:" << event->mask << std::endl;
        }
      } else {
        std::cout << "Fn:" << __func__ << ":" << __LINE__
                  << " vale:" << fn.extension().string()
                  << " comp:" << fn.extension().string().compare(".lua")
                  << std::endl;
      }
    }

    // sizeof(struct inotify_event) will return the size of fixed data type
    // and will not include size of variable array's size as it's not known at
    // compile time, that's why + event->len to cater variable string length
    offset += sizeof(struct inotify_event) + event->len;
  } while (offset <= in.length());

  // case Where, file is moved away from watched location
  if (!m_old_event.empty()) {
    auto *old_event =
        reinterpret_cast<struct inotify_event *>(m_old_event.data());
    std::cout << "Fn:" << __func__ << ":" << __LINE__
              << " old-file-name:" << old_event->name << std::endl;
    // clear old event now
    m_old_event.clear();
  }
  return (offset);
}

std::int32_t fs_app::handle_read(const std::int32_t &channel,
                                 const std::string &in, const bool &dry_run) {
  if (dry_run) {
    return 0;
  }

  auto offset = process_inotify_onchange(in);
  return (offset);
}

std::int32_t fs_app::handle_event(const std::int32_t &channel,
                                  const std::uint16_t &event) {
  return 0;
}

std::int32_t fs_app::handle_write(const std::int32_t &channel) { return 0; }
std::int32_t fs_app::handle_close(const std::int32_t &channel) { return 0; }

#endif
