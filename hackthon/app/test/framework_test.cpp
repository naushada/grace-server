// Tests for the base framework — evt_base singleton and the default virtual
// implementations on evt_io. We construct evt_io with a real fd obtained
// via socketpair() so libevent's bufferevent_socket_new() succeeds; we
// never dispatch the event loop, so callbacks are not actually fired.

#include "framework.hpp"

#include <gtest/gtest.h>

extern "C" {
#include <sys/socket.h>
#include <unistd.h>
}

namespace {

class FrameworkTest : public ::testing::Test {
protected:
  int sv[2] = {-1, -1};

  void SetUp() override {
    ASSERT_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  }

  void TearDown() override {
    // sv[0] (if still >= 0) was not transferred to a bufferevent; close it.
    // sv[1] is always ours to close.
    if (sv[0] >= 0)
      close(sv[0]);
    if (sv[1] >= 0)
      close(sv[1]);
  }
};

TEST_F(FrameworkTest, EvtBaseSingletonIsUsable) {
  EXPECT_TRUE(static_cast<bool>(evt_base::instance()));
  EXPECT_NE(evt_base::instance().get(), nullptr);
  // The static instance must be stable across calls.
  EXPECT_EQ(evt_base::instance().get(), evt_base::instance().get());
}

TEST_F(FrameworkTest, EvtIoDefaultHandlersReturnZero) {
  evt_io io(sv[0]);
  sv[0] = -1; // bufferevent owns the fd now (BEV_OPT_CLOSE_ON_FREE)

  EXPECT_EQ(io.handle_read(0, "payload", /*dry_run=*/true), 0);
  EXPECT_EQ(io.handle_event(0, 0), 0);
  EXPECT_EQ(io.handle_write(0), 0);
  EXPECT_EQ(io.handle_connect(0, "host"), 0);
  EXPECT_EQ(io.handle_close(0), 0);
  EXPECT_EQ(io.handle_accept(0, "host"), 0);
}

TEST_F(FrameworkTest, GetBufferEventReturnsNonNull) {
  evt_io io(sv[0]);
  sv[0] = -1;
  EXPECT_NE(io.get_bufferevt(), nullptr);
}

TEST_F(FrameworkTest, TwoArgConstructorAcceptsPeerHost) {
  evt_io io(sv[0], "10.1.2.3");
  sv[0] = -1;
  EXPECT_NE(io.get_bufferevt(), nullptr);
}

} // namespace
