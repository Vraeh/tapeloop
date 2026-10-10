cmake_minimum_required(VERSION 3.28...3.30)

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/common/bootstrap.cmake" NO_POLICY_SCOPE)

project(${_name} VERSION ${_version})

option(ENABLE_FRONTEND_API "Use obs-frontend-api for UI functionality" ON)
option(ENABLE_QT "Use Qt functionality" ON)

include(compilerconfig)
include(defaults)
include(helpers)

find_package(libobs REQUIRED)

add_subdirectory(src/core)
add_subdirectory(src/obs)

add_library(${CMAKE_PROJECT_NAME} MODULE)

target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::libobs tapeloop-core tapeloop-obs)

# Decoding uses a private, static FFmpeg; macOS will decode with VideoToolbox instead.
if(NOT APPLE)
  include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ffmpeg/ffmpeg.cmake")
  tapeloop_add_ffmpeg()
  add_subdirectory(src/decode)
  target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE tapeloop-decode)
  target_compile_definitions(${CMAKE_PROJECT_NAME} PRIVATE TAPELOOP_FFMPEG)
  # obs-deps' FFmpeg headers are on every target that links libobs, so every file that
  # includes FFmpeg includes decode/FFmpegHeaders.hpp first, which stops the build on
  # them.
  file(
    GLOB_RECURSE ffmpeg_users
    "${CMAKE_CURRENT_SOURCE_DIR}/src/*.[ch]pp"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/*.[ch]pp"
  )
  foreach(file IN LISTS ffmpeg_users)
    file(READ "${file}" text)
    string(FIND "${text}" "#include <libav" ffmpeg)
    string(FIND "${text}" "#include \"decode/FFmpegHeaders.hpp\"" guard)
    if(NOT ffmpeg EQUAL -1 AND NOT file MATCHES "FFmpegHeaders\\.hpp$" AND (guard EQUAL -1 OR guard GREATER ffmpeg))
      message(FATAL_ERROR "${file} includes FFmpeg without decode/FFmpegHeaders.hpp before it")
    endif()
  endforeach()
  get_property(ffmpeg_notices GLOBAL PROPERTY TAPELOOP_FFMPEG_NOTICES)
  foreach(notice IN LISTS ffmpeg_notices)
    target_add_resource(${CMAKE_PROJECT_NAME} "${notice}")
  endforeach()
  # OBS loads its own FFmpeg into the same process; ours stays inside the plugin. Only
  # FFmpeg's archives: hiding every archive would also hide the runtimes some
  # sanitizers link statically.
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(${CMAKE_PROJECT_NAME} PRIVATE "LINKER:--exclude-libs=libavformat.a:libavcodec.a:libavutil.a")
  endif()
endif()

# The capture manager lives on the frontend's events and scene collections.
if(NOT ENABLE_FRONTEND_API)
  message(FATAL_ERROR "Tapeloop needs ENABLE_FRONTEND_API")
endif()
find_package(obs-frontend-api REQUIRED)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE OBS::obs-frontend-api)

# The dock is a Qt widget.
if(NOT ENABLE_QT)
  message(FATAL_ERROR "Tapeloop needs ENABLE_QT")
endif()
find_package(Qt6 REQUIRED COMPONENTS Widgets Core)
target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE Qt6::Core Qt6::Widgets)
target_compile_options(
  ${CMAKE_PROJECT_NAME}
  PRIVATE $<$<C_COMPILER_ID:Clang,AppleClang>:-Wno-quoted-include-in-framework-header -Wno-comma>
)
set_target_properties(
  ${CMAKE_PROJECT_NAME}
  PROPERTIES AUTOMOC ON AUTOUIC ON AUTORCC ON
)

# FrontendBridge needs obs-frontend-api, which the libobs tests do not have, so it is
# not part of tapeloop-obs.
target_sources(
  ${CMAKE_PROJECT_NAME}
  PRIVATE
    src/obs/FrontendBridge.cpp
    src/obs/FrontendBridge.hpp
    src/plugin-main.cpp
    src/ui/DockBackend.hpp
    src/ui/DockText.cpp
    src/ui/DockText.hpp
    src/ui/SourceSettingsDialog.cpp
    src/ui/SourceSettingsDialog.hpp
    src/ui/TapeloopDock.cpp
    src/ui/TapeloopDock.hpp
)

set_target_properties_plugin(${CMAKE_PROJECT_NAME} PROPERTIES OUTPUT_NAME ${_name})
