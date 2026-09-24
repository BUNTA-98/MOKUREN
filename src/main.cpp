#include "mokuren.hpp"
#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
  // verlangt jetzt 2 pfade aus dem terminal
  if (argc < 3) {
    std::cerr << "fehler: pfade fehlen.\n"
              << "nutzung: " << argv[0] << " <daten_pfad> <config_pfad>\n";
    return 1;
  }

  std::string target_path = argv[1];
  std::string config_path = argv[2]; // pfad zur config.json

  Mokuren engine;
  engine.RunGridSearch(target_path, config_path);

  return 0;
}