#include "tui_dashboard.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
  mokuren::tui::TuiDashboard::Options opts;

  for (int i = 1; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--host" && i + 1 < argc) {
      opts.redis_host = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      opts.redis_port = std::atoi(argv[++i]);
    } else if (arg == "--fps" && i + 1 < argc) {
      opts.fps = std::atoi(argv[++i]);
    } else if (arg == "--help" || arg == "-h") {
      std::printf(
          "usage: tui_dashboard [--host HOST] [--port PORT] [--fps N]\n");
      return 0;
    }
  }

  mokuren::tui::TuiDashboard dashboard(opts);
  return dashboard.Run();
}
