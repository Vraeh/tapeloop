function(tapeloop_core_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  else()
    target_compile_options(
      ${target}
      PRIVATE -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    )
  endif()
endfunction()

add_library(tapeloop-core STATIC)

target_sources(
  tapeloop-core
  PRIVATE
    BufferLifecycle.cpp
    BufferLifecycle.hpp
    BufferSettings.cpp
    BufferSettings.hpp
    Clip.cpp
    Clip.hpp
    DecodePlanner.cpp
    DecodePlanner.hpp
    EncoderPolicy.cpp
    EncoderPolicy.hpp
    FrameDecoder.hpp
    Gop.cpp
    Gop.hpp
    MediaTime.cpp
    MediaTime.hpp
    MomentCutter.cpp
    MomentCutter.hpp
    MomentList.cpp
    MomentList.hpp
    Playhead.cpp
    Playhead.hpp
    ReplaySize.cpp
    ReplaySize.hpp
    SourceBuffer.cpp
    SourceBuffer.hpp
)

target_include_directories(tapeloop-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(tapeloop-core PUBLIC cxx_std_20)
set_target_properties(tapeloop-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
tapeloop_core_warnings(tapeloop-core)
