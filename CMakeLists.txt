cmake_minimum_required(VERSION 3.14)
project(mokuren)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

if(POLICY CMP0115)
  cmake_policy(SET CMP0115 NEW)
endif()

# 1. system-bibliotheken (threads & pkgconfig für notcurses)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

include(FetchContent)

# 2. json-bibliothek
FetchContent_Declare(
  json 
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

# 3. notcurses c++ binding finden
pkg_check_modules(NOTCURSES REQUIRED IMPORTED_TARGET notcurses++)

# Sammelt automatisch alle .cpp Dateien im src-Ordner
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")

# 4. executable mit all deinen source-dateien
add_executable(mokuren ${SOURCES})

# 5. include ordner bekanntmachen
target_include_directories(mokuren PRIVATE include)

# 6. alle bibliotheken verlinken
target_link_libraries(mokuren PRIVATE 
  Threads::Threads 
  nlohmann_json::nlohmann_json
  PkgConfig::NOTCURSES
)
