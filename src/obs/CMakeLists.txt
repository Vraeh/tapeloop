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
    ManagerDockBackend.cpp
    ManagerDockBackend.hpp
    ObsEncoders.cpp
    ObsEncoders.hpp
    PictureRenderer.cpp
    PictureRenderer.hpp
    RenderAdapter.cpp
    RenderAdapter.hpp
    SettingsData.cpp
    SettingsData.hpp
    SourceCapture.cpp
    SourceCapture.hpp
)

# Pictures the decoder keeps on the GPU reach OBS through shared textures only on
# Direct3D 11.
if(WIN32)
  target_sources(tapeloop-obs PRIVATE SharedPictures.cpp SharedPictures.hpp)
endif()

target_include_directories(tapeloop-obs PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(tapeloop-obs PUBLIC tapeloop-core OBS::libobs)
set_target_properties(tapeloop-obs PROPERTIES POSITION_INDEPENDENT_CODE ON)
tapeloop_core_warnings(tapeloop-obs)
