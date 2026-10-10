# The FFmpeg Tapeloop decodes and exports with, linked statically: built once by
# BuildFFmpeg.cmake into .deps at the top of the repository, in a directory keyed on that
# script's hash and the compiler, so a new version, configure line or compiler builds
# again and anything else reuses it.
# tapeloop_add_ffmpeg() defines the imported targets FFmpeg::avformat, FFmpeg::avcodec and
# FFmpeg::avutil, and the global property TAPELOOP_FFMPEG_NOTICES: the notice and license
# files a package that links them carries.
include_guard(GLOBAL)

set(_tapeloop_ffmpeg_dir "${CMAKE_CURRENT_LIST_DIR}")

# The libraries a static FFmpeg library needs, from the Libs line of its pkg-config file
# (a static build lists them there rather than in Libs.private).
function(_tapeloop_ffmpeg_link_libraries pc_file own_library out_variable)
  file(STRINGS "${pc_file}" line REGEX "^Libs:")
  string(REGEX REPLACE "^Libs:[ ]*" "" line "${line}")
  separate_arguments(tokens NATIVE_COMMAND "${line}")
  set(libraries)
  foreach(token IN LISTS tokens)
    if(token STREQUAL "-l${own_library}" OR token MATCHES "^-L")
      continue()
    elseif(token STREQUAL "-pthread")
      find_package(Threads REQUIRED)
      list(APPEND libraries Threads::Threads)
    elseif(token MATCHES "^-l(.+)$")
      list(APPEND libraries "${CMAKE_MATCH_1}")
    elseif(token MATCHES "\\.lib$")
      list(APPEND libraries "${token}")
    endif()
  endforeach()
  set(${out_variable} "${libraries}" PARENT_SCOPE)
endfunction()

function(tapeloop_add_ffmpeg)
  if(TARGET FFmpeg::avcodec)
    return()
  endif()

  set(script "${_tapeloop_ffmpeg_dir}/BuildFFmpeg.cmake")
  # On Windows FFmpeg is built with the plugin's own Visual Studio, SDK and toolset,
  # which are then part of the key too: objects of a newer compiler than the linker do
  # not link.
  set(toolchain)
  if(MSVC)
    if(CMAKE_GENERATOR_INSTANCE)
      list(APPEND toolchain "-DVISUAL_STUDIO=${CMAKE_GENERATOR_INSTANCE}")
    endif()
    if(CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION)
      list(APPEND toolchain "-DWINDOWS_SDK=${CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION}")
    endif()
    if(CMAKE_VS_PLATFORM_TOOLSET_VERSION)
      list(APPEND toolchain "-DVC_TOOLSET=${CMAKE_VS_PLATFORM_TOOLSET_VERSION}")
    endif()
  endif()
  file(SHA256 "${script}" script_hash)
  string(SHA256 key "${script_hash};${toolchain};${CMAKE_C_COMPILER_ID};${CMAKE_C_COMPILER_VERSION}")
  string(SUBSTRING "${key}" 0 16 key)
  cmake_path(ABSOLUTE_PATH _tapeloop_ffmpeg_dir NORMALIZE OUTPUT_VARIABLE repository)
  cmake_path(GET repository PARENT_PATH repository)
  cmake_path(GET repository PARENT_PATH repository)
  set(prefix "${repository}/.deps/ffmpeg-${key}")

  if(NOT EXISTS "${prefix}/configure-line.txt")
    message(STATUS "Building FFmpeg into ${prefix}")
    execute_process(
      COMMAND
        "${CMAKE_COMMAND}" "-DPREFIX=${prefix}" "-DWORK_DIR=${repository}/.deps/ffmpeg-${key}-work" ${toolchain} -P
        "${script}"
      RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Building FFmpeg failed")
    endif()
  endif()
  set_property(
    GLOBAL
    PROPERTY
      TAPELOOP_FFMPEG_NOTICES
        "${prefix}/FFmpeg-NOTICE.txt"
        "${prefix}/FFmpeg-LICENSE.txt"
        "${prefix}/FFmpeg-LICENSE.md"
        "${prefix}/FFmpeg-THIRD-PARTY.txt"
        "${_tapeloop_ffmpeg_dir}/FFmpeg-IJG-README-6b.txt"
        "${_tapeloop_ffmpeg_dir}/FFmpeg-IJG-README-4.txt"
  )
  file(READ "${prefix}/configure-line.txt" configure_line)
  string(STRIP "${configure_line}" configure_line)
  message(STATUS "FFmpeg from ${prefix}: ${configure_line}")

  foreach(library IN ITEMS avutil avcodec avformat)
    add_library(FFmpeg::${library} STATIC IMPORTED GLOBAL)
    _tapeloop_ffmpeg_link_libraries("${prefix}/lib/pkgconfig/lib${library}.pc" ${library} libraries)
    if(library STREQUAL "avcodec")
      list(PREPEND libraries FFmpeg::avutil)
    elseif(library STREQUAL "avformat")
      list(PREPEND libraries FFmpeg::avcodec FFmpeg::avutil)
    endif()
    # MSVC builds name their static libraries avcodec.lib, the others libavcodec.a.
    set(location "${prefix}/lib/lib${library}.a")
    if(EXISTS "${prefix}/lib/${library}.lib")
      set(location "${prefix}/lib/${library}.lib")
    endif()
    set_target_properties(
      FFmpeg::${library}
      PROPERTIES
        IMPORTED_LOCATION "${location}"
        INTERFACE_INCLUDE_DIRECTORIES "${prefix}/include"
        # FFmpeg's headers refuse to compile as C++ without it.
        INTERFACE_COMPILE_DEFINITIONS __STDC_CONSTANT_MACROS
        INTERFACE_LINK_LIBRARIES "${libraries}"
    )
  endforeach()
endfunction()
