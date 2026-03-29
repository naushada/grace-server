#ifndef __client_app_hpp__
#define __client_app_hpp__

#include "framework.hpp"

class server;

class connected_client : public evt_io {
public:
  connected_client(const std::int32_t &channel, const std::string &peer_host,
                   server *parent)
      : evt_io(channel, peer_host), m_parent(parent) {}

  virtual ~connected_client() {
    std::cout << "Fn:" << __func__ << ":" << __LINE__ << " dtor" << std::endl;
  }

  server &parent() const { return *m_parent; }
  virtual std::int32_t handle_read(const std::int32_t &channel,
                                   const std::string &data,
                                   const bool &dry_run) override;
  virtual std::int32_t handle_event(const std::int32_t &channel,
                                    const std::uint16_t &event) override;
  virtual std::int32_t handle_write(const std::int32_t &channel) override;
  virtual std::int32_t handle_close(const std::int32_t &channel) override;

private:
  server *m_parent;
};
#endif // !__client_app_hpp__
