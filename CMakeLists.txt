cmake_minimum_required(VERSION 3.14)
project(KAITO CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# FetchContent lädt FTXUI automatisch von GitHub herunter
include(FetchContent)

FetchContent_Declare(
  ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/ftxui
  GIT_TAG v5.0.0
)
FetchContent_MakeAvailable(ftxui)

# Header-Verzeichnis angeben
include_directories(include)

# Sucht die Thread-Bibliothek deines Betriebssystems
find_package(Threads REQUIRED)

# Executable definieren und Quelldateien auflisten
add_executable(
  mokuren
  src/main.cpp
  src/aggregator.cpp
  src/ui.cpp
  src/csv_parser.cpp
)

# Verbindet dein Programm fest mit den CPU-Threads (Füge Threads::Threads hinzu!)
target_link_libraries(mokuren PRIVATE Threads::Threads)

# FTXUI-Module mit unserem Programm verknüpfen
target_link_libraries(
  mokuren
  PRIVATE ftxui::screen
          ftxui::dom
          ftxui::component
)
