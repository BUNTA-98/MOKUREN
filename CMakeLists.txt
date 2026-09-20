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

# Executable definieren und Quelldateien auflisten
add_executable(
  kaito
  src/csv_test.cpp
  src/aggregator.cpp
  src/ui.cpp
  src/csv_parser.cpp
)

# FTXUI-Module mit unserem Programm verknüpfen
target_link_libraries(
  kaito
  PRIVATE ftxui::screen
          ftxui::dom
          ftxui::component
)
