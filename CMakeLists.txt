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

# 1. thread-bibliothek
find_package(Threads REQUIRED)

include(FetchContent)

# 2. json-bibliothek
FetchContent_Declare(
  json 
  URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(json)

# 3. ftxui-bibliothek (wiederhergestellt)
FetchContent_Declare(ftxui
  GIT_REPOSITORY https://github.com/ArthurSonzogni/FTXUI
  GIT_TAG main
)
FetchContent_MakeAvailable(ftxui)

# 4. executable mit all deinen source-dateien
add_executable(mokuren src/main.cpp src/aggregator.cpp src/csv_parser.cpp)

# 5. include ordner bekanntmachen
target_include_directories(mokuren PRIVATE include)

# 6. alle bibliotheken verlinken
target_link_libraries(mokuren PRIVATE 
  Threads::Threads 
  nlohmann_json::nlohmann_json
  ftxui::component
  ftxui::dom
  ftxui::screen
)
