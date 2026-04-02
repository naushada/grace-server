#ifndef __fs_app_cpp__
#define __fs_app_cpp__

#include "fs_app.hpp"

std::int32_t fs_app::handle_read(const std::int32_t &channel,
                                 const std::string &in, const bool &dry_run) {
  if (dry_run) {
    return 0;
  }

  auto offset = 0;
  do {
    struct inotify_event *event = (struct inotify_event *)(in.data() + offset);
    if (event->len) {
      // This is a file
      namespace fs = std::filesystem;
      std::string fname(event->name);
      fs::path fn(fname);
      std::cout << "Fn:" << __func__ << ":" << __LINE__
                << " file-name: " << fname << " ext:" << fn.extension()
                << std::endl;
      // fn.extension() will return extention with double quotes within, hence
      // using string to get rid of this.
      if (fn.extension().string() == std::string(".lua")) {
        if (event->mask & (IN_CREATE | IN_MODIFY | IN_MOVED_TO)) {
          // Insert into MAP - key=filename
          // lua_engine().update_command(fname);
          std::cout << "Fn:" << __func__ << ":" << __LINE__
                    << " This file:" << fname << " is created" << std::endl;

        } else if (event->mask & (IN_DELETE | IN_MOVED_FROM)) {
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
    offset += sizeof(struct inotify_event) + event->len;
  } while (offset <= in.length());
  return (offset);
}

std::int32_t fs_app::handle_event(const std::int32_t &channel,
                                  const std::uint16_t &event) {
  return 0;
}

std::int32_t fs_app::handle_write(const std::int32_t &channel) { return 0; }
std::int32_t fs_app::handle_close(const std::int32_t &channel) { return 0; }

#endif
