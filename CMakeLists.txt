cmake_minimum_required(VERSION 3.20)
project(ratelimitd LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

option(RATELIMIT_WERROR "Treat warnings as errors" ON)
set(RATELIMIT_SANITIZE "" CACHE STRING "Sanitizers to enable: address,undefined | thread")

find_package(Threads REQUIRED)

add_library(ratelimit_warnings INTERFACE)
target_compile_options(ratelimit_warnings INTERFACE -Wall -Wextra -Wpedantic -Wshadow -Wconversion
                                                    -Wno-sign-conversion)
if(RATELIMIT_WERROR)
  target_compile_options(ratelimit_warnings INTERFACE -Werror)
endif()

if(RATELIMIT_SANITIZE)
  add_compile_options(-fsanitize=${RATELIMIT_SANITIZE} -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=${RATELIMIT_SANITIZE})
endif()

add_library(ratelimit
  src/limiter.cc
  src/resp.cc
  src/commands.cc
  src/server.cc)
target_include_directories(ratelimit PUBLIC include)
target_link_libraries(ratelimit PUBLIC Threads::Threads PRIVATE ratelimit_warnings)

add_executable(ratelimitd src/main.cc)
target_link_libraries(ratelimitd PRIVATE ratelimit ratelimit_warnings)

add_executable(bench_limiter bench/bench_limiter.cc)
target_link_libraries(bench_limiter PRIVATE ratelimit ratelimit_warnings)

enable_testing()
add_executable(ratelimit_tests
  tests/main.cc
  tests/gcra_test.cc
  tests/limiter_test.cc
  tests/resp_test.cc
  tests/commands_test.cc
  tests/server_test.cc)
target_link_libraries(ratelimit_tests PRIVATE ratelimit ratelimit_warnings)
add_test(NAME ratelimit_tests COMMAND ratelimit_tests)
