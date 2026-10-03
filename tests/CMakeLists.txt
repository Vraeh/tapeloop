cmake_minimum_required(VERSION 3.28...3.30)

project(tapeloop-tests LANGUAGES CXX)

option(TAPELOOP_FUZZ "Build the libFuzzer harnesses (Clang only)" OFF)

if(TAPELOOP_FUZZ)
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "The fuzz harnesses need LLVM Clang, which ships libFuzzer")
  endif()
  # Everything is instrumented for libFuzzer's coverage feedback and runs under ASan and
  # UBSan; only the harnesses link libFuzzer itself.
  add_compile_options(
    -fsanitize=fuzzer-no-link,address,undefined
    -fno-sanitize-recover=undefined
    -fno-omit-frame-pointer
  )
  add_link_options(-fsanitize=address,undefined)
endif()

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
    core/ClipTests.cpp
    core/EncoderPolicyTests.cpp
    core/GopTests.cpp
    core/MediaTimeTests.cpp
    core/MomentCutterTests.cpp
    core/MomentListTests.cpp
    core/PlayheadTests.cpp
    core/SourceBufferTests.cpp
    core/SyntheticEncoder.hpp
)
target_link_libraries(tapeloop-core-tests PRIVATE tapeloop-core Catch2::Catch2WithMain)
tapeloop_core_warnings(tapeloop-core-tests)

enable_testing()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(tapeloop-core-tests)

if(TAPELOOP_FUZZ)
  add_subdirectory(fuzz)
endif()
