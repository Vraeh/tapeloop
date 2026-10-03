cmake_minimum_required(VERSION 3.28...3.30)

project(tapeloop-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

FetchContent_Declare(
  Catch2
  URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz
  URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
  SYSTEM
)
FetchContent_MakeAvailable(Catch2)

# Warnings inside Catch2 are not ours to fix.
set_target_properties(Catch2 Catch2WithMain PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

add_subdirectory(../src/core core)

add_executable(tapeloop-core-tests)
target_sources(
  tapeloop-core-tests
  PRIVATE
    core/AllocationCounter.cpp
    core/AllocationCounter.hpp
    core/GopTests.cpp
    core/MediaTimeTests.cpp
    core/SyntheticEncoder.hpp
)
target_link_libraries(tapeloop-core-tests PRIVATE tapeloop-core Catch2::Catch2WithMain)
tapeloop_core_warnings(tapeloop-core-tests)

enable_testing()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(tapeloop-core-tests)
