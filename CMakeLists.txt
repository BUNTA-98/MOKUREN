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

# system libs
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)

include(FetchContent)

# json lib
FetchContent_Declare(
  json 
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

# spdlog (async logging)
FetchContent_Declare(
  spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG        v1.14.1
)
FetchContent_MakeAvailable(spdlog)

# notcurses bindings
pkg_check_modules(NOTCURSES REQUIRED IMPORTED_TARGET notcurses++)

# ixwebsocket (live market data)
FetchContent_Declare(
  ixwebsocket
  GIT_REPOSITORY https://github.com/machinezone/IXWebSocket.git
  GIT_TAG        v11.4.5
)
set(USE_TLS ON CACHE BOOL "" FORCE)
set(USE_OPEN_SSL ON CACHE BOOL "" FORCE)
set(IXWEBSOCKET_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(ixwebsocket)

# collect source files
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")

# main executable
add_executable(mokuren ${SOURCES})

# include dirs
target_include_directories(mokuren PRIVATE include)

# link libs
target_link_libraries(mokuren PRIVATE 
  Threads::Threads 
  nlohmann_json::nlohmann_json
  PkgConfig::NOTCURSES
  spdlog::spdlog
  ixwebsocket
)

# --- test environment ---
include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.2
)
FetchContent_MakeAvailable(Catch2)

enable_testing()

# collect test files
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "test/*.cpp")

# test executable
add_executable(engine_tests ${TEST_SOURCES})

# collect engine code without main.cpp
file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(REMOVE_ITEM ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

# attach engine code and include dirs
target_sources(engine_tests PRIVATE ${ENGINE_SOURCES})
target_include_directories(engine_tests PRIVATE src include)

# link test libs
target_link_libraries(engine_tests PRIVATE 
  Catch2::Catch2WithMain
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  ixwebsocket
)
