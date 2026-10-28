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

# market data format i/o, named explicitly on top of the glob.
#
# engine_tests used a non-recursive glob, so src/market_data/*.cpp was never in
# its source list: the files compiled into `mokuren` and not into the tests, and
# the only symptom was undefined mokuren::mdf::* at link time -- three steps from
# the cause. keep these listed so a stale configure cannot hide them again;
# remove-then-append keeps each file compiled exactly once.
set(MARKET_DATA_SOURCES
  ${CMAKE_CURRENT_SOURCE_DIR}/src/market_data/reader.cpp
  ${CMAKE_CURRENT_SOURCE_DIR}/src/market_data/writer.cpp
)
list(REMOVE_ITEM SOURCES ${MARKET_DATA_SOURCES})
list(APPEND SOURCES ${MARKET_DATA_SOURCES})

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

# collect engine code without main.cpp. RECURSE, matching the main target: the
# non-recursive glob silently dropped every source under src/market_data/, and a
# header-only include made that look like a linking problem rather than a
# missing compilation unit.
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS "src/*.cpp")
list(REMOVE_ITEM ENGINE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/src/main.cpp")
list(REMOVE_ITEM ENGINE_SOURCES ${MARKET_DATA_SOURCES})
list(APPEND ENGINE_SOURCES ${MARKET_DATA_SOURCES})

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

# --- market data recorder ---
#
# a separate binary, not a --record flag on `mokuren`. recording is not trading,
# and a switch that can turn a live session into a recorder mid-run is a way to
# lose the session. it links the engine sources minus main.cpp on purpose: the
# recording has to be produced by the parser that trades, or it is evidence about
# a different program.
add_executable(mokuren_record tools/record.cpp ${ENGINE_SOURCES} ${HEADERS})

target_include_directories(mokuren_record PRIVATE
  include
  ${ixwebsocket_SOURCE_DIR}
)

target_link_libraries(mokuren_record PRIVATE
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

# --- dataset verification ---
#
# reads a dataset through DataManager, i.e. through the same two calls the
# replay makes, and prints the span it got back. the point is to catch the gap
# between "the file decodes" and "the backtest can use it": wrong units, an
# empty channel, an order the Slice* helpers rely on. linking the engine
# sources (not a trimmed reader-only subset) is what makes the answer apply to
# the binary that trades.
add_executable(mokuren_inspect tools/inspect.cpp ${ENGINE_SOURCES} ${HEADERS})

target_include_directories(mokuren_inspect PRIVATE
  include
  ${ixwebsocket_SOURCE_DIR}
)

target_link_libraries(mokuren_inspect PRIVATE
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

# --- dataset merging ---
#
# merges a directory of daily .mkn files into weekly/monthly files. separate
# binary, same reasoning as record/inspect: a data tool, not a trading path, so
# it must not become a switch on the binary that trades. links the engine
# sources rather than a reader-only subset, so the merge is produced by the
# same parser the backtest runs on.
add_executable(mokuren_merge tools/merge.cpp ${ENGINE_SOURCES} ${HEADERS})

target_include_directories(mokuren_merge PRIVATE
  include
  ${ixwebsocket_SOURCE_DIR}
)

target_link_libraries(mokuren_merge PRIVATE
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
