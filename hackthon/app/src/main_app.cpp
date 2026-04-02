#ifndef __main_app_cpp__
#define __main_app_cpp__

#include "client_app.hpp"
#include "framework.hpp"
#include "fs_app.hpp"
#include "server_app.hpp"

int main(std::int32_t argc, const char *argv[]) {

  fs_app fs_mon("/app/command");
  server svc_module("0.0.0.0", 58989);
  std::cout << "Server started on port:58989" << std::endl;
  run_evt_loop main_loop;

  main_loop();

  return 0;
}
#endif //__main_app_cpp__
