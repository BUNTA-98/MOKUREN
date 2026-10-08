#include "logger.hpp"
#include "ui_manager.hpp"
#include <iostream>

int main(int argc, char** argv) {
  mokuren::InitLogger();
  LOG_INFO("engine starting up");

  try {
    UIManager ui;
    ui.Run();
  } catch (const std::exception& e) {
    LOG_ERROR("fataler fehler: {}", e.what());
    std::cerr << "fataler fehler: " << e.what() << "\n";
    mokuren::ShutdownLogger();
    return 1;
  }

  LOG_INFO("engine shutting down");
  mokuren::ShutdownLogger();
  return 0;
}
