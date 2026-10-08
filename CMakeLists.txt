cmake_minimum_required(VERSION 3.20)
project(eventforge CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "EventForge is Linux only (epoll, futex, /dev/shm). On a Mac use the Dockerfile.")
endif()

option(DAQ_BUILD_TESTS "Build unit + e2e tests" ON)
option(DAQ_SANITIZE "ASan + UBSan" OFF)
option(DAQ_TSAN "ThreadSanitizer" OFF)

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow)
if(DAQ_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()
if(DAQ_TSAN)
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)

# shared helpers
add_library(daq_common STATIC
  src/common/crc32c.cpp
  src/common/histogram.cpp
  src/common/metrics.cpp
  src/common/net.cpp
  src/common/ttc.cpp
)
target_include_directories(daq_common PUBLIC include src)
target_link_libraries(daq_common PUBLIC Threads::Threads rt)

# builder pieces (everything but main, so tests can use them)
add_library(daq_builder_lib STATIC
  src/builder/assembler.cpp
  src/builder/receiver.cpp
  src/builder/shard.cpp
  src/builder/sink.cpp
  src/builder/sweeper.cpp
  src/builder/trigger.cpp
)
target_link_libraries(daq_builder_lib PUBLIC daq_common)

add_library(daq_frontend_lib STATIC
  src/frontend/faults.cpp
  src/frontend/trigger.cpp
)
target_link_libraries(daq_frontend_lib PUBLIC daq_common)

add_executable(daq_builder src/builder/main.cpp)
target_link_libraries(daq_builder PRIVATE daq_builder_lib)

add_executable(daq_frontend src/frontend/main.cpp)
target_link_libraries(daq_frontend PRIVATE daq_frontend_lib)

if(DAQ_BUILD_TESTS)
  find_package(GTest QUIET)
  if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(googletest
      GIT_REPOSITORY https://github.com/google/googletest.git
      GIT_TAG v1.15.2
      GIT_SHALLOW TRUE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
  endif()

  enable_testing()
  add_executable(unit_tests
    tests/test_wire.cpp
    tests/test_crc32c.cpp
    tests/test_histogram.cpp
    tests/test_queue.cpp
    tests/test_ttc.cpp
    tests/test_source_tracker.cpp
    tests/test_assembler.cpp
  )
  target_link_libraries(unit_tests PRIVATE daq_builder_lib daq_frontend_lib GTest::gtest_main)

  # e2e spawns the real binaries
  add_executable(e2e_tests tests/test_e2e.cpp)
  target_link_libraries(e2e_tests PRIVATE daq_common GTest::gtest_main)
  target_compile_definitions(e2e_tests PRIVATE DAQ_BIN_DIR="${CMAKE_BINARY_DIR}")
  add_dependencies(e2e_tests daq_builder daq_frontend)

  include(GoogleTest)
  gtest_discover_tests(unit_tests)
  gtest_discover_tests(e2e_tests PROPERTIES TIMEOUT 120)
endif()
