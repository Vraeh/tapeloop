cmake_minimum_required(VERSION 3.28...3.30)

# Tests of the libobs glue against a real, headless libobs. BuildLibobs.cmake builds
# that libobs; TAPELOOP_LIBOBS_PREFIX points at where it installed it.
project(tapeloop-obs-tests LANGUAGES C CXX)

set(TAPELOOP_LIBOBS_PREFIX "" CACHE PATH "Prefix BuildLibobs.cmake installed libobs into")
if(NOT EXISTS "${TAPELOOP_LIBOBS_PREFIX}/lib/cmake/libobs/libobsConfig.cmake")
  message(FATAL_ERROR "TAPELOOP_LIBOBS_PREFIX must point at a libobs built by BuildLibobs.cmake")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_PREFIX_PATH "${TAPELOOP_LIBOBS_PREFIX}")
find_package(libobs REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavcodec libavutil)

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

add_subdirectory(../../src/core core)
add_subdirectory(../../src/obs obs)

# A module that calls back into the test from obs_module_unload.
add_library(tapeloop-test-unload MODULE unload-hook/UnloadHook.cpp)
target_link_libraries(tapeloop-test-unload PRIVATE OBS::libobs)
set_target_properties(tapeloop-test-unload PROPERTIES PREFIX "")

add_executable(tapeloop-obs-tests)
target_sources(
  tapeloop-obs-tests
  PRIVATE
    BufferOutput.cpp
    BufferOutput.hpp
    CaptureTests.cpp
    ClipDecoder.cpp
    ClipDecoder.hpp
    ObsFixture.cpp
    ObsFixture.hpp
    SmokeTests.cpp
    TestEncoders.cpp
    TestEncoders.hpp
    TestPattern.cpp
    TestPattern.hpp
)
target_compile_definitions(
  tapeloop-obs-tests
  PRIVATE
    TAPELOOP_LIBOBS_PREFIX="${TAPELOOP_LIBOBS_PREFIX}"
    TAPELOOP_TEST_UNLOAD_MODULE="$<TARGET_FILE:tapeloop-test-unload>"
)
add_dependencies(tapeloop-obs-tests tapeloop-test-unload)
target_link_libraries(
  tapeloop-obs-tests
  PRIVATE tapeloop-core tapeloop-obs OBS::libobs PkgConfig::FFMPEG Catch2::Catch2WithMain
)
set_target_properties(tapeloop-obs-tests PROPERTIES BUILD_RPATH "${TAPELOOP_LIBOBS_PREFIX}/lib")
tapeloop_core_warnings(tapeloop-obs-tests)

enable_testing()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(tapeloop-obs-tests)
