cmake_minimum_required(VERSION 3.20)
project(twitch_core LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(TWITCH_BUILD_CURL "Build the libcurl platform layer and twitch-cli" ON)

find_package(nlohmann_json 3.11 REQUIRED)

add_library(twitch_core
  src/url.cpp
  src/gql.cpp
  src/streams.cpp
  src/browse.cpp
  src/hls.cpp
  src/login.cpp
  src/playback.cpp
  src/videos.cpp
  src/irc.cpp
  src/chat.cpp
  src/stream_websocket.cpp
  src/chat_log.cpp
  src/third_party_emotes.cpp
  src/badges.cpp
  src/settings.cpp
  src/progress.cpp
  src/history.cpp
  src/notify.cpp
  src/auth.cpp
  src/helix.cpp
  src/qr.cpp
  third_party/qrcodegen/qrcodegen.cpp
)
target_include_directories(twitch_core PUBLIC include)
target_include_directories(twitch_core PRIVATE third_party)
target_link_libraries(twitch_core PUBLIC nlohmann_json::nlohmann_json)
target_compile_options(twitch_core PRIVATE -Wall -Wextra -Wpedantic)

if(TWITCH_BUILD_CURL)
  find_package(CURL 8.0 REQUIRED)
  add_library(twitch_core_curl
    platform/curl/src/curl_http.cpp
    platform/curl/src/curl_websocket.cpp
  )
  target_include_directories(twitch_core_curl PUBLIC platform/curl/include)
  target_link_libraries(twitch_core_curl PUBLIC twitch_core CURL::libcurl)
  target_compile_options(twitch_core_curl PRIVATE -Wall -Wextra -Wpedantic)

  add_executable(twitch-cli cli/twitch_cli.cpp)
  target_link_libraries(twitch-cli PRIVATE twitch_core_curl)
endif()

# Unit tests, when tests/ is there.
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt")
  option(TWITCH_BUILD_TESTS "Build unit tests" ON)
  if(TWITCH_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
  endif()
endif()
