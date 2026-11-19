cmake_minimum_required(VERSION 3.16)
project(kvcache LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic)

option(KV_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(KV_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
endif()

add_library(kvcache_storage src/block_pool.cpp src/block_table.cpp)
target_include_directories(kvcache_storage PUBLIC include)
add_executable(kvcache src/main.cpp)
target_link_libraries(kvcache PRIVATE kvcache_storage)

add_executable(block_demo examples/block_demo.cpp)
target_link_libraries(block_demo PRIVATE kvcache_storage)

include(CTest)
if(BUILD_TESTING)
  add_executable(storage_tests tests/storage_tests.cpp)
  target_link_libraries(storage_tests PRIVATE kvcache_storage)
  add_test(NAME storage_tests COMMAND storage_tests)
  add_test(NAME block_demo_golden COMMAND ${CMAKE_COMMAND}
    -DPROGRAM=$<TARGET_FILE:block_demo>
    -DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/block_demo.in
    -DEXPECTED=${CMAKE_CURRENT_SOURCE_DIR}/tests/golden/block_demo.expected
    -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/check_demo.cmake)
endif()

# --- Manager (Yingtao). Additive: no line above is changed. ---
add_library(kvcache_manager src/config.cpp src/kv_cache_manager.cpp)
target_link_libraries(kvcache_manager PUBLIC kvcache_storage)

if(BUILD_TESTING)
  add_executable(manager_tests tests/manager_tests.cpp)
  target_link_libraries(manager_tests PRIVATE kvcache_manager)
  add_test(NAME manager_tests COMMAND manager_tests)
endif()
