#ifndef __client_app_cpp__
#define __client_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"

std::int32_t connected_client::handle_read(const std::int32_t &channel,
                                           const std::string &data,
                                           const bool &dry_run) {
  if (dry_run) {
    return 0;
  }
  return (data.length());
}

std::int32_t connected_client::handle_event(const std::int32_t &channel,
                                            const std::uint16_t &event) {
  return (0);
}

std::int32_t connected_client::handle_write(const std::int32_t &channel) {
  return (0);
}

std::int32_t connected_client::handle_close(const std::int32_t &channel) {
  return (0);
}

#endif
