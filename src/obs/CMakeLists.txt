cmake_minimum_required(VERSION 3.28...3.30)

# The glue between the core and libobs. The plugin and the libobs tests link it; the
# caller provides the OBS::libobs target.
add_library(tapeloop-obs STATIC)

target_sources(
  tapeloop-obs
  PRIVATE
    CaptureManager.cpp
    CaptureManager.hpp
    CaptureOutput.cpp
    CaptureOutput.hpp
    ObsEncoders.cpp
    ObsEncoders.hpp
    SettingsData.cpp
    SettingsData.hpp
    SourceCapture.cpp
    SourceCapture.hpp
)

target_include_directories(tapeloop-obs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(tapeloop-obs PUBLIC tapeloop-core OBS::libobs)
set_target_properties(tapeloop-obs PROPERTIES POSITION_INDEPENDENT_CODE ON)
tapeloop_core_warnings(tapeloop-obs)
