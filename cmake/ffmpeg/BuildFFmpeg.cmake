# Builds the FFmpeg that Tapeloop links statically for decoding: the H.264 and HEVC
# decoders and parsers and nothing else, under the LGPL (no --enable-gpl, no
# --enable-version3), and on Windows the D3D11VA hwaccels. Run in script mode:
#
#   cmake -DPREFIX=<install dir> -DWORK_DIR=<build dir> -P BuildFFmpeg.cmake
#
# Linux and macOS hosts use their shell and make; Windows uses MSVC and the shell, make
# and nasm of MSYS2 (C:/msys64, or MSYS2_ROOT). nasm is needed everywhere. On Windows,
# -DVISUAL_STUDIO=<installation>, -DWINDOWS_SDK=<version> and -DVC_TOOLSET=<version>
# pick the compiler the plugin uses; without them, the newest Visual Studio and its
# defaults. The version and the options below are the build's identity:
# cmake/ffmpeg/ffmpeg.cmake keys the install directory on this file's hash.
cmake_minimum_required(VERSION 3.28)

set(version 8.1.3)
set(url "https://ffmpeg.org/releases/ffmpeg-${version}.tar.xz")
# The tarball's GPG signature was checked against FFmpeg's release signing key
# (FCF9 86EA 15E6 E293 A564 4F10 B432 2F04 D676 58D8) when this version was pinned.
set(sha256 7138d28c96d9d3e3af4ee3d8cad72741f8ffb40da90c1112235dea3ecd3178a3)

# --disable-autodetect turns threads off too, so each platform names its own.
set(
  options
  --disable-everything
  --disable-programs
  --disable-doc
  --disable-network
  --disable-autodetect
  --disable-avformat
  --disable-avfilter
  --disable-avdevice
  --disable-swscale
  --disable-swresample
  --enable-decoder=h264,hevc
  --enable-parser=h264,hevc
  --enable-static
  --disable-shared
)
if(CMAKE_HOST_WIN32)
  # -MD: the C runtime OBS and the plugin use.
  list(
    APPEND
    options
    --toolchain=msvc
    --enable-w32threads
    --enable-d3d11va
    --enable-hwaccel=h264_d3d11va2,hevc_d3d11va2
    --extra-cflags=-MD
  )
else()
  # -fPIC: the static libraries end up in a shared object.
  list(APPEND options --enable-pthreads --enable-pic)
endif()

# Files of FFmpeg that carry a license other than the LGPL, alone or next to it, among
# everything the build above compiles or includes. Each comes with that license and
# a text the notice starts at, the start of the comment that holds that text being
# taken. Their notices go into FFmpeg-THIRD-PARTY.txt.
set(
  other_licenses
  "libavcodec/aom_film_grain_template.c|BSD-2-Clause|Redistribution and use"
  "libavcodec/faandct.c|ISC|Permission to use"
  "libavcodec/jfdctfst.c|IJG|Independent JPEG Group"
  "libavcodec/jfdctint_template.c|IJG|Independent JPEG Group"
  "libavcodec/jrevdct.c|IJG|Independent JPEG Group"
  "libavutil/adler32.c|zlib|provided 'as-is'"
  "libavutil/avsscanf.c|MIT|Permission is hereby granted"
  "libavutil/fixed_dsp.c|BSD-3-Clause|Redistribution and use"
  "libavutil/fixed_dsp.h|BSD-3-Clause|Redistribution and use"
  "libavutil/uuid.c|BSD-3-Clause|Redistribution and use"
  "libavutil/x86/x86inc.asm|ISC|Permission to use"
)
# Files whose permissive text asks for nothing in a binary: code after Boost's
# algorithms, whose license covers object code without its notice, and credits to
# public-domain code.
set(reviewed_licenses "libavutil/libm.h" "libavutil/mathematics.c" "libavutil/sha.c")
# What marks a notice other than the LGPL's.
set(
  other_license_marks
  "Redistribution and use in source and binary forms|Permission is hereby granted|Permission to use, copy, modify|provided 'as-is'|Independent JPEG Group|Boost Software License|[Pp]ublic domain|Apache License"
)

# -DPRINT_SOURCE=ON prints the version and the source URL, for the release notes, and
# builds nothing.
if(PRINT_SOURCE)
  message("${version} ${url}")
  return()
endif()

# -DDOWNLOAD_SOURCE=<dir> puts the source tarball, checked against its SHA-256, into
# that directory, for the release to carry, and builds nothing.
if(DOWNLOAD_SOURCE)
  set(archive "${DOWNLOAD_SOURCE}/ffmpeg-${version}.tar.xz")
  # A release should not fail for one dropped connection; a wrong hash is not retried.
  foreach(attempt RANGE 1 3)
    file(DOWNLOAD "${url}" "${archive}" TLS_VERIFY ON STATUS status)
    list(GET status 0 code)
    if(code EQUAL 0)
      file(SHA256 "${archive}" actual)
      if(NOT actual STREQUAL sha256)
        file(REMOVE "${archive}")
        message(FATAL_ERROR "${url} has SHA-256 ${actual}, not ${sha256}")
      endif()
      message("${archive}")
      return()
    endif()
    file(REMOVE "${archive}")
    message(STATUS "Downloading ${url} failed (${attempt} of 3): ${status}")
  endforeach()
  message(FATAL_ERROR "Downloading ${url} failed")
endif()

foreach(variable IN ITEMS PREFIX WORK_DIR)
  if(NOT ${variable})
    message(FATAL_ERROR "BuildFFmpeg.cmake needs -D${variable}=<path>")
  endif()
  # FFmpeg's configure refuses an out-of-tree build whose paths contain whitespace.
  if(${variable} MATCHES "[ \t]")
    message(FATAL_ERROR "FFmpeg cannot be built under a path with spaces: ${${variable}}")
  endif()
endforeach()

file(REMOVE_RECURSE "${WORK_DIR}" "${PREFIX}")
file(MAKE_DIRECTORY "${WORK_DIR}/build")
set(archive "${WORK_DIR}/ffmpeg-${version}.tar.xz")
message(STATUS "Downloading FFmpeg ${version}")
file(DOWNLOAD "${url}" "${archive}" EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS status)
list(GET status 0 code)
if(NOT code EQUAL 0)
  message(FATAL_ERROR "Downloading ${url} failed: ${status}")
endif()
file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${WORK_DIR}")
set(source "${WORK_DIR}/ffmpeg-${version}")

cmake_host_system_information(RESULT jobs QUERY NUMBER_OF_LOGICAL_CORES)

if(CMAKE_HOST_WIN32)
  # FFmpeg's configure needs cl, link and lib on the PATH and the Windows SDK's include
  # and library paths, which vcvars64.bat sets for the batch file that runs each step;
  # MSYS2_PATH_TYPE=inherit hands them to the MSYS2 shell.
  if(VISUAL_STUDIO)
    set(visual_studio "${VISUAL_STUDIO}")
  else()
    execute_process(
      COMMAND
        "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires
        Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
      OUTPUT_VARIABLE visual_studio
      OUTPUT_STRIP_TRAILING_WHITESPACE
      COMMAND_ERROR_IS_FATAL ANY
    )
  endif()
  file(TO_NATIVE_PATH "${visual_studio}/VC/Auxiliary/Build/vcvars64.bat" vcvars)
  set(vcvars_arguments)
  if(WINDOWS_SDK)
    string(APPEND vcvars_arguments " ${WINDOWS_SDK}")
  endif()
  if(VC_TOOLSET)
    string(APPEND vcvars_arguments " -vcvars_ver=${VC_TOOLSET}")
  endif()
  set(msys "C:/msys64")
  if(DEFINED ENV{MSYS2_ROOT})
    file(TO_CMAKE_PATH "$ENV{MSYS2_ROOT}" msys)
  endif()
  file(TO_NATIVE_PATH "${msys}/usr/bin/bash.exe" bash)
  execute_process(
    COMMAND "${msys}/usr/bin/cygpath.exe" -u "${source}"
    OUTPUT_VARIABLE shell_source
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  execute_process(
    COMMAND "${msys}/usr/bin/cygpath.exe" -u "${PREFIX}"
    OUTPUT_VARIABLE shell_prefix
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
else()
  set(shell_source "${source}")
  set(shell_prefix "${PREFIX}")
endif()

# Runs a shell command in the build directory, its output in <step>.log.
function(run_step step command)
  set(log "${WORK_DIR}/${step}.log")
  if(CMAKE_HOST_WIN32)
    set(batch "${WORK_DIR}/${step}.bat")
    file(
      WRITE
      "${batch}"
      "@call \"${vcvars}\"${vcvars_arguments} >NUL || exit /b 1\r\n"
      "@set MSYS2_PATH_TYPE=inherit\r\n"
      "@set CHERE_INVOKING=1\r\n"
      "@\"${bash}\" -lc \"${command}\"\r\n"
    )
    execute_process(
      COMMAND cmd /c "${batch}"
      WORKING_DIRECTORY "${WORK_DIR}/build"
      OUTPUT_FILE "${log}"
      ERROR_FILE "${log}"
      RESULT_VARIABLE result
    )
  else()
    execute_process(
      COMMAND sh -c "${command}"
      WORKING_DIRECTORY "${WORK_DIR}/build"
      OUTPUT_FILE "${log}"
      ERROR_FILE "${log}"
      RESULT_VARIABLE result
    )
  endif()
  if(NOT result EQUAL 0)
    file(READ "${log}" text)
    # configure says little on stdout; why it stopped is at the end of its own log.
    if(step STREQUAL "configure" AND EXISTS "${WORK_DIR}/build/ffbuild/config.log")
      file(READ "${WORK_DIR}/build/ffbuild/config.log" config_log)
      string(APPEND text "\n--- ffbuild/config.log ---\n${config_log}")
    endif()
    string(LENGTH "${text}" length)
    if(length GREATER 20000)
      math(EXPR start "${length} - 20000")
      string(SUBSTRING "${text}" ${start} 20000 text)
    endif()
    message(FATAL_ERROR "FFmpeg ${step} failed:\n${text}")
  endif()
endfunction()

list(JOIN options " " joined)
message(STATUS "Configuring FFmpeg ${version}: ${joined}")
run_step(configure "'${shell_source}/configure' --prefix='${shell_prefix}' ${joined}")

# configure drops a requested component without a word when a dependency is missing, as
# the HEVC hwaccel does without DXVA_PicParams_HEVC; what it kept is in config.h and
# config_components.h.
set(
  required
  CONFIG_H264_DECODER
  CONFIG_HEVC_DECODER
  CONFIG_H264_PARSER
  CONFIG_HEVC_PARSER
  HAVE_THREADS
)
if(CMAKE_HOST_WIN32)
  list(APPEND required CONFIG_H264_D3D11VA2_HWACCEL CONFIG_HEVC_D3D11VA2_HWACCEL)
endif()
file(READ "${WORK_DIR}/build/config.h" config_h)
file(READ "${WORK_DIR}/build/config_components.h" components_h)
string(APPEND config_h "${components_h}")
foreach(name IN LISTS required)
  if(NOT config_h MATCHES "#define ${name} 1")
    message(FATAL_ERROR "FFmpeg's configure left out ${name}")
  endif()
endforeach()

message(STATUS "Building FFmpeg ${version} with ${jobs} jobs")
run_step(build "make -j${jobs} && make install")

# Every file the build compiled or included, from the dependency files the compiler
# and the assembler wrote, has its license checked: one with a notice other than the
# LGPL's that the lists above do not name stops the build. Paths are compared from the
# source directory down, without case, as Windows writes them.
set(listed)
foreach(entry IN LISTS other_licenses)
  string(REPLACE "|" ";" entry "${entry}")
  list(GET entry 0 path)
  string(TOLOWER "${path}" path)
  list(APPEND listed "${path}")
endforeach()
foreach(path IN LISTS reviewed_licenses)
  string(TOLOWER "${path}" path)
  list(APPEND listed "${path}")
endforeach()
file(GLOB_RECURSE dependency_files "${WORK_DIR}/build/*.d")
if(NOT dependency_files)
  message(FATAL_ERROR "FFmpeg's build left no dependency files to check the licenses of")
endif()
set(used)
foreach(dependency_file IN LISTS dependency_files)
  file(READ "${dependency_file}" dependencies)
  # Line continuations go, then Windows separators and doubled ones, which nasm writes;
  # FFmpeg names its sources through the src link it makes in the build directory, or
  # by their full path.
  string(REPLACE "\\\r\n" " " dependencies "${dependencies}")
  string(REPLACE "\\\n" " " dependencies "${dependencies}")
  string(REPLACE "\\" "/" dependencies "${dependencies}")
  string(REGEX REPLACE "/+" "/" dependencies "${dependencies}")
  string(REGEX REPLACE "[ \t\r\n]+" ";" tokens "${dependencies}")
  foreach(token IN LISTS tokens)
    if(token MATCHES "^src/([^:]+)")
      list(APPEND used "${CMAKE_MATCH_1}")
    elseif(token MATCHES "ffmpeg-${version}/([^:]+)")
      list(APPEND used "${CMAKE_MATCH_1}")
    endif()
  endforeach()
endforeach()
list(REMOVE_DUPLICATES used)
if(NOT "libavcodec/h264dec.c" IN_LIST used)
  message(FATAL_ERROR "FFmpeg's dependency files do not name its sources the way this script reads them")
endif()
set(unlisted)
foreach(path IN LISTS used)
  if(NOT EXISTS "${source}/${path}")
    continue()
  endif()
  file(READ "${source}/${path}" text)
  string(TOLOWER "${path}" key)
  if(text MATCHES "${other_license_marks}" AND NOT key IN_LIST listed)
    list(APPEND unlisted "${path}")
  endif()
endforeach()
if(unlisted)
  list(JOIN unlisted "\n  " unlisted)
  message(FATAL_ERROR "Not under the LGPL alone, and in neither other_licenses nor reviewed_licenses:\n  ${unlisted}")
endif()

# What a package that links FFmpeg statically carries next to it.
file(COPY_FILE "${source}/COPYING.LGPLv2.1" "${PREFIX}/FFmpeg-LICENSE.txt")
file(COPY_FILE "${source}/LICENSE.md" "${PREFIX}/FFmpeg-LICENSE.md")
file(
  WRITE
  "${PREFIX}/FFmpeg-NOTICE.txt"
  "Tapeloop includes FFmpeg ${version}, linked statically, under the GNU Lesser General\n"
  "Public License version 2.1 or later. The license is in FFmpeg-LICENSE.txt, FFmpeg's\n"
  "own account of its licensing in FFmpeg-LICENSE.md, and the notices of the few files\n"
  "under other licenses in FFmpeg-THIRD-PARTY.txt.\n"
  "\n"
  "Source: ${url}\n"
  "SHA-256: ${sha256}\n"
  "\n"
  "Built from that source without changes, configured with:\n"
  "${joined}\n"
  "\n"
  "Every release of Tapeloop carries that FFmpeg source and Tapeloop's own source. With\n"
  "them you can build Tapeloop again against a modified FFmpeg and use it in place of\n"
  "this one, as the LGPL allows: point url and sha256 in cmake/ffmpeg/BuildFFmpeg.cmake\n"
  "at your FFmpeg's tarball (a file:// URL will do) and build Tapeloop as its README\n"
  "says.\n"
)
string(
  CONCAT
  third_party
  "Most of FFmpeg is under the LGPL. A few of the files this build of it compiles, or\n"
  "includes into what it compiles, also carry other licenses; their notices follow, as\n"
  "they stand in FFmpeg ${version}'s source.\n"
  "\n"
  "This software is based in part on the work of the Independent JPEG Group. FFmpeg's\n"
  "jfdctfst.c, jfdctint_template.c and jrevdct.c are its changed copies of libjpeg's\n"
  "jfdctfst.c, jfdctint.c and jrevdct.c; FFmpeg's history of them is at\n"
  "https://git.ffmpeg.org/ffmpeg.git.\n"
)
foreach(entry IN LISTS other_licenses)
  string(REPLACE "|" ";" entry "${entry}")
  list(GET entry 0 path)
  list(GET entry 1 license)
  list(GET entry 2 mark)
  file(READ "${source}/${path}" text)
  string(FIND "${text}" "${mark}" at)
  if(at EQUAL -1)
    message(FATAL_ERROR "${path} no longer holds \"${mark}\"")
  endif()
  # The comment around the mark: a C block, or the run of lines starting with ";*" in
  # an assembly file.
  string(SUBSTRING "${text}" 0 ${at} before)
  string(SUBSTRING "${text}" ${at} -1 after)
  if(path MATCHES "\\.asm$")
    string(REGEX MATCH "(;\\*[^\n]*\n)*;\\*[^\n]*$" opening "${before}")
    string(REGEX MATCH "^[^\n]*\n(;\\*[^\n]*\n)*" closing "${after}")
  else()
    string(FIND "${before}" "/*" start REVERSE)
    string(FIND "${after}" "*/" end)
    if(start EQUAL -1 OR end EQUAL -1)
      message(FATAL_ERROR "${path} holds \"${mark}\" outside a comment")
    endif()
    string(SUBSTRING "${before}" ${start} -1 opening)
    math(EXPR end "${end} + 2")
    string(SUBSTRING "${after}" 0 ${end} closing)
    string(APPEND closing "\n")
  endif()
  string(APPEND third_party "\n${path} (${license}):\n\n${opening}${closing}")
endforeach()
file(WRITE "${PREFIX}/FFmpeg-THIRD-PARTY.txt" "${third_party}")

# Written last: ffmpeg.cmake takes the prefix as complete only once this file is there.
file(WRITE "${PREFIX}/configure-line.txt" "${joined}\n")
file(REMOVE_RECURSE "${WORK_DIR}")
