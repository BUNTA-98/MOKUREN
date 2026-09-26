#include "ui_manager.hpp"
#include <iostream>

int main(int argc, char** argv) {
  try {
    UIManager ui;
    ui.Run();
  } catch (const std::exception& e) {
    std::cerr << "fataler fehler: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
