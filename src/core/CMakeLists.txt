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
    Crc32c.cpp
    Crc32c.hpp
    DecodePlanner.cpp
    DecodePlanner.hpp
    EncoderPolicy.cpp
    EncoderPolicy.hpp
    FailurePacer.cpp
    FailurePacer.hpp
    FileIo.cpp
    FileIo.hpp
    FrameDecoder.hpp
    Gop.cpp
    Gop.hpp
    GopReader.cpp
    GopReader.hpp
    MediaTime.cpp
    MediaTime.hpp
    MomentCutter.cpp
    MomentCutter.hpp
    ParameterSets.cpp
    ParameterSets.hpp
    Playhead.cpp
    Playhead.hpp
    ReplayFormat.cpp
    ReplayFormat.hpp
    ReplayLibrary.cpp
    ReplayLibrary.hpp
    ReplayNames.cpp
    ReplayNames.hpp
    ReplayPlayerState.cpp
    ReplayPlayerState.hpp
    ReplaySequence.cpp
    ReplaySequence.hpp
    ReplaySize.cpp
    ReplaySize.hpp
    ReplayState.hpp
    ReplayStore.cpp
    ReplayStore.hpp
    ReplayWriter.cpp
    ReplayWriter.hpp
    SourceBuffer.cpp
    SourceBuffer.hpp
)

target_include_directories(tapeloop-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/..")
target_compile_features(tapeloop-core PUBLIC cxx_std_20)
set_target_properties(tapeloop-core PROPERTIES POSITION_INDEPENDENT_CODE ON)
tapeloop_core_warnings(tapeloop-core)
