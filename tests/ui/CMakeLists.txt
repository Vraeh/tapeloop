cmake_minimum_required(VERSION 3.28...3.30)

# Tests of the dock's widgets with Qt's offscreen platform. The widgets reach OBS only
# through DockBackend, so these tests need no libobs.
project(tapeloop-ui-tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Qt6 REQUIRED COMPONENTS Widgets)

include(FetchContent)

FetchContent_Declare(
  Catch2
  URL https://github.com/catchorg/Catch2/archive/refs/tags/v3.16.0.tar.gz
  URL_HASH SHA256=0957cae5821b17ce07f0833aaa52b5137643a8382203221f363a8303c109af34
  SYSTEM
)
FetchContent_MakeAvailable(Catch2)

# Warnings inside Catch2 are not ours to fix.
set_target_properties(Catch2 PROPERTIES COMPILE_WARNING_AS_ERROR OFF)

add_subdirectory(../../src/core core)

add_executable(tapeloop-ui-tests)
target_sources(
  tapeloop-ui-tests
  PRIVATE
    ../../src/ui/DockBackend.hpp
    ../../src/ui/DockText.cpp
    ../../src/ui/DockText.hpp
    ../../src/ui/SourceSettingsDialog.cpp
    ../../src/ui/SourceSettingsDialog.hpp
    ../../src/ui/TapeloopDock.cpp
    ../../src/ui/TapeloopDock.hpp
    DockTests.cpp
    FakeBackend.hpp
    LocaleText.cpp
    LocaleText.hpp
    main.cpp
)
target_include_directories(tapeloop-ui-tests PRIVATE ../../src)
target_compile_definitions(
  tapeloop-ui-tests
  PRIVATE TAPELOOP_LOCALE_FILE="${CMAKE_CURRENT_SOURCE_DIR}/../../data/locale/en-US.ini"
)
target_link_libraries(tapeloop-ui-tests PRIVATE tapeloop-core Qt6::Widgets Catch2::Catch2)
tapeloop_core_warnings(tapeloop-ui-tests)

enable_testing()
list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
include(Catch)
catch_discover_tests(tapeloop-ui-tests)
