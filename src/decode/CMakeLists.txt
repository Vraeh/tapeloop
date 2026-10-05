cmake_minimum_required(VERSION 3.28...3.30)

# Decoding with FFmpeg, without libobs. The caller provides the FFmpeg targets through
# tapeloop_add_ffmpeg().
add_library(tapeloop-decode STATIC)

target_sources(
  tapeloop-decode
  PRIVATE FFmpegDecoder.cpp FFmpegDecoder.hpp FFmpegVersion.cpp FFmpegVersion.hpp Picture.hpp
)

target_include_directories(tapeloop-decode PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_link_libraries(tapeloop-decode PUBLIC tapeloop-core PRIVATE FFmpeg::avcodec)
if(WIN32)
  target_link_libraries(tapeloop-decode PRIVATE d3d11 dxgi)
endif()
set_target_properties(tapeloop-decode PROPERTIES POSITION_INDEPENDENT_CODE ON)
tapeloop_core_warnings(tapeloop-decode)
