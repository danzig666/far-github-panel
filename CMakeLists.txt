cmake_minimum_required(VERSION 3.20)
project(far-github-panel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Static CRT on MSVC so the plugin DLL has no runtime dependencies.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(GHP_BUILD_TESTS "Build the unit tests" ON)
option(GHP_BUILD_LIVE_TESTS "Build the tests that call api.github.com" ON)
if(WIN32)
  option(GHP_BUILD_PLUGIN "Build the Far Manager plugin DLL" ON)
endif()

if(MSVC)
  add_compile_options(/W4 /utf-8 /permissive- /EHsc)
else()
  add_compile_options(-Wall -Wextra)
endif()

# Platform-neutral core: JSON, URLs, GitHub API client, tree model.
add_library(ghcore STATIC
  src/core/GitHubClient.cpp
  src/core/Json.cpp
  src/core/Location.cpp
  src/core/RepoTree.cpp
  src/core/Text.cpp
  src/core/Url.cpp
)
target_include_directories(ghcore PUBLIC src/core)

if(WIN32)
  # WinHTTP transport, shared by the plugin and the Windows live tests.
  add_library(ghwinhttp STATIC src/far/WinHttpTransport.cpp)
  target_compile_definitions(ghwinhttp PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_libraries(ghwinhttp PUBLIC ghcore winhttp)
endif()

if(GHP_BUILD_TESTS)
  enable_testing()
  add_executable(unit_tests tests/unit_tests.cpp)
  target_link_libraries(unit_tests PRIVATE ghcore)
  add_test(NAME unit_tests COMMAND unit_tests)
endif()

if(GHP_BUILD_LIVE_TESTS)
  if(WIN32)
    add_executable(live_tests tests/live_tests.cpp)
    target_link_libraries(live_tests PRIVATE ghwinhttp)
  else()
    find_package(CURL)
    if(CURL_FOUND)
      add_executable(live_tests tests/live_tests.cpp tests/CurlHttp.cpp)
      target_link_libraries(live_tests PRIVATE ghcore CURL::libcurl)
    else()
      message(STATUS "libcurl not found: live_tests skipped")
    endif()
  endif()
endif()

if(WIN32 AND GHP_BUILD_PLUGIN)
  add_library(GitHubPanel SHARED
    src/far/Config.cpp
    src/far/FarApi.cpp
    src/far/Panel.cpp
    src/far/Plugin.cpp
    src/far/Progress.cpp
    src/far/Token.cpp
    src/far/GitHubPanel.def
    src/far/GitHubPanel.rc
  )
  target_include_directories(GitHubPanel PRIVATE far-sdk src/far)
  target_link_libraries(GitHubPanel PRIVATE ghwinhttp crypt32)
  set_target_properties(GitHubPanel PROPERTIES PREFIX "" OUTPUT_NAME "GitHubPanel")
endif()
