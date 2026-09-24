#include "mokuren.hpp"
#include <iostream>
#include <string>

int main(int argc, char *argv[]) {
  // Terminal Argumente abfangen (genau wie vorher)[cite: 3]
  if (argc < 2) {
    std::cerr << "fehler: zielpfad (csv oder ordner) fehlt.\n"
              << "nutzung: " << argv[0] << " <pfad>\n";
    return 1;
  }

  std::string target_path = argv[1];

  // Engine hochfahren und Zielpfad übergeben
  Mokuren engine;
  engine.RunGridSearch(target_path);

  return 0;
}