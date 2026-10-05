cmake_minimum_required(VERSION 3.28...3.30)

# Tests of the libobs glue against a real, headless libobs. On Linux BuildLibobs.cmake
# builds that libobs and TAPELOOP_LIBOBS_PREFIX points at where it installed it. On
# Windows the plugin's own configure builds it into .deps, with the Direct3D 11 renderer
# and obs-x264 when given -DTAPELOOP_OBS_TEST_RUNTIME=ON, and the modules are taken from
# that build's rundir.
project(tapeloop-obs-tests LANGUAGES C CXX)

set(TAPELOOP_LIBOBS_PREFIX "" CACHE PATH "Prefix libobs was installed into")
if(WIN32)
  set(TAPELOOP_OBS_RUNDIR "" CACHE PATH "Release rundir of the OBS build, with obs-x264")
  set(TAPELOOP_OBS_DEPS_DIR "" CACHE PATH "obs-deps, whose x264 DLL obs-x264 loads")
  if(
    NOT EXISTS "${TAPELOOP_LIBOBS_PREFIX}/cmake/libobs/libobsConfig.cmake"
    OR NOT EXISTS "${TAPELOOP_OBS_RUNDIR}/obs-plugins/64bit/obs-x264.dll"
  )
    message(FATAL_ERROR "Configure the plugin with -DTAPELOOP_OBS_TEST_RUNTIME=ON first")
  endif()
  list(APPEND CMAKE_PREFIX_PATH "${TAPELOOP_OBS_DEPS_DIR}")
  set(_obs_plugin_dir "${TAPELOOP_OBS_RUNDIR}/obs-plugins/64bit")
  set(_obs_plugin_data_dir "${TAPELOOP_OBS_RUNDIR}/data/obs-plugins")
  set(_obs_graphics_module "libobs-d3d11")
else()
  if(NOT EXISTS "${TAPELOOP_LIBOBS_PREFIX}/lib/cmake/libobs/libobsConfig.cmake")
    message(FATAL_ERROR "TAPELOOP_LIBOBS_PREFIX must point at a libobs built by BuildLibobs.cmake")
  endif()
  set(_obs_plugin_dir "${TAPELOOP_LIBOBS_PREFIX}/lib/obs-plugins")
  set(_obs_plugin_data_dir "${TAPELOOP_LIBOBS_PREFIX}/share/obs/obs-plugins")
  set(_obs_graphics_module "${TAPELOOP_LIBOBS_PREFIX}/lib/libobs-opengl")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

list(APPEND CMAKE_PREFIX_PATH "${TAPELOOP_LIBOBS_PREFIX}")
find_package(libobs REQUIRED)

# The decoder of the clips is the plugin's own FFmpeg, not the system's.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ffmpeg/ffmpeg.cmake")
tapeloop_add_ffmpeg()

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
add_subdirectory(../../src/decode decode)

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
    DecodeTests.cpp
    DockBackendTests.cpp
    ManagerTests.cpp
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
    TAPELOOP_OBS_PLUGIN_DIR="${_obs_plugin_dir}"
    TAPELOOP_OBS_PLUGIN_DATA_DIR="${_obs_plugin_data_dir}"
    TAPELOOP_OBS_GRAPHICS_MODULE="${_obs_graphics_module}"
    TAPELOOP_OBS_MODULE_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}"
    TAPELOOP_TEST_UNLOAD_MODULE="$<TARGET_FILE:tapeloop-test-unload>"
)
add_dependencies(tapeloop-obs-tests tapeloop-test-unload)
target_link_libraries(
  tapeloop-obs-tests
  PRIVATE tapeloop-core tapeloop-obs tapeloop-decode OBS::libobs FFmpeg::avcodec Catch2::Catch2WithMain
)
# libobs loads the system's FFmpeg; the static one stays inside the test binary. Only
# FFmpeg's archives: Clang links the sanitizer runtimes as archives that must stay
# exported. A Windows executable exports nothing unless asked to.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(tapeloop-obs-tests PRIVATE "LINKER:--exclude-libs=libavcodec.a:libavutil.a")
endif()
set_target_properties(tapeloop-obs-tests PROPERTIES BUILD_RPATH "${TAPELOOP_LIBOBS_PREFIX}/lib")
tapeloop_core_warnings(tapeloop-obs-tests)

enable_testing()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
if(WIN32)
  # libobs looks for its effects at ../../data/libobs from the working directory.
  catch_discover_tests(
    tapeloop-obs-tests
    WORKING_DIRECTORY "${TAPELOOP_OBS_RUNDIR}/bin/64bit"
    DL_PATHS "${TAPELOOP_OBS_RUNDIR}/bin/64bit" "${TAPELOOP_OBS_DEPS_DIR}/bin"
  )
else()
  catch_discover_tests(tapeloop-obs-tests)
endif()
