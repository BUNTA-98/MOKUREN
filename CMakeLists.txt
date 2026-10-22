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

# OpenSSL (HMAC-SHA256 signing for the Bybit REST/WS auth)
find_package(OpenSSL REQUIRED)

# libcurl (authenticated REST calls to the Bybit V5 API)
find_package(CURL REQUIRED)

# hiredis (redis pub/sub for live event broadcasting)
pkg_check_modules(HIREDIS REQUIRED IMPORTED_TARGET hiredis)

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

# collect source files. headers are tracked too: with the sources glob alone an
# edit to a header never re-triggered a build of the targets that include it.
file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS "src/*.cpp")
file(GLOB_RECURSE HEADERS CONFIGURE_DEPENDS "include/*.hpp")

# main executable
add_executable(mokuren ${SOURCES} ${HEADERS})

# include dirs
# ${ixwebsocket_SOURCE_DIR} is added explicitly so <ixwebsocket/IXWebSocket.h>
# resolves even if the fetched target does not re-export its build interface.
target_include_directories(mokuren PRIVATE include ${ixwebsocket_SOURCE_DIR})

# link libs
target_link_libraries(mokuren PRIVATE 
  Threads::Threads 
  nlohmann_json::nlohmann_json
  PkgConfig::NOTCURSES
  spdlog::spdlog
  ixwebsocket
  PkgConfig::HIREDIS
  OpenSSL::SSL
  OpenSSL::Crypto
  CURL::libcurl
)

# --- test environment ---
include(FetchContent)

FetchContent_Declare(
  Catch2
  GIT_REPOSITORY https://github.com/catchorg/Catch2.git
  GIT_TAG        v3.5.2
)
FetchContent_MakeAvailable(Catch2)

# no enable_testing() / no ctest: this project has exactly one test runner and
# it is the Catch2 binary (Catch2WithMain) at build/engine_tests. registering
# the same cases with a second runner meant two sources of truth for pass/fail,
# and an exit code CI could read from either.
#
# collect test files
file(GLOB_RECURSE TEST_SOURCES CONFIGURE_DEPENDS "test/*.cpp")

# test executable
add_executable(engine_tests ${TEST_SOURCES})

# collect engine code without main.cpp
file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(REMOVE_ITEM ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")

# attach engine code and include dirs
target_sources(engine_tests PRIVATE ${ENGINE_SOURCES} ${HEADERS})
target_include_directories(engine_tests PRIVATE src include ${ixwebsocket_SOURCE_DIR})

# link test libs
target_link_libraries(engine_tests PRIVATE 
  Catch2::Catch2WithMain
  nlohmann_json::nlohmann_json
  spdlog::spdlog
  ixwebsocket
  PkgConfig::HIREDIS
  OpenSSL::SSL
  OpenSSL::Crypto
  CURL::libcurl
)

# run with ./build/engine_tests (or ./build/engine_tests "*router*" to narrow).
#
# the [.live] cases are opt-in and hidden to Catch2, so the plain run never
# touches a socket:
#   ./build/engine_tests "[.live]"
# there is deliberately no make target wrapping that. a target named after a
# diagnostic reads like a second program, and it is not one: the operator-facing
# version of this check is `mokuren` itself, which runs the same gate on start,
# prints the same table, and refuses to arm on failure.
# NOTE: the startup probe used to be a separate `venue_doctor` executable. it
# is not one any more, on purpose. its checks now live in
# include/data_integrity.hpp, include/startup_gate.hpp and
# include/feed_router.hpp -- sources picked up by the src/*.cpp glob above --
# and are exercised by test/test_data_integrity.cpp under engine_tests. one
# implementation, compiled into the same binary that trades, so the preflight
# verdict and the live gate can never disagree about what "valid" means.
