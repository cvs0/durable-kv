cmake_minimum_required(VERSION 3.16)
project(durable-kv LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(durable_kv
  src/format.cpp
  src/os_file.cpp
  src/store.cpp
)
target_include_directories(durable_kv
  PUBLIC include
  PRIVATE src
)
if(MSVC)
  target_compile_options(durable_kv PRIVATE /W4 /permissive-)
  target_compile_definitions(durable_kv PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(durable_kv PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(durable-kv src/cli.cpp)
target_link_libraries(durable-kv PRIVATE durable_kv)
if(MSVC)
  target_compile_options(durable-kv PRIVATE /W4 /permissive-)
else()
  target_compile_options(durable-kv PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_executable(durable_kv_tests tests/store_test.cpp)
target_link_libraries(durable_kv_tests PRIVATE durable_kv)
target_include_directories(durable_kv_tests PRIVATE src)
if(MSVC)
  target_compile_options(durable_kv_tests PRIVATE /W4 /permissive-)
  target_compile_definitions(durable_kv_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
  target_compile_options(durable_kv_tests PRIVATE -Wall -Wextra -Wpedantic)
endif()

enable_testing()
add_test(NAME durable_kv_tests COMMAND durable_kv_tests)
