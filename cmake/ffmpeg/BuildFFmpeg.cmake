# Builds the FFmpeg that Tapeloop links statically for decoding: the H.264 and HEVC
# decoders and parsers and nothing else, under the LGPL (no --enable-gpl, no
# --enable-version3), and on Windows the D3D11VA hwaccels. Run in script mode:
#
#   cmake -DPREFIX=<install dir> -DWORK_DIR=<build dir> -P BuildFFmpeg.cmake
#
# Linux and macOS hosts use their shell and make; Windows uses MSVC from the newest
# Visual Studio and the shell, make and nasm of MSYS2 (C:/msys64, or MSYS2_ROOT). nasm is
# needed everywhere. The version and the options below are the build's identity:
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

# -DPRINT_SOURCE=ON prints the version and the source URL, for the release notes, and
# builds nothing.
if(PRINT_SOURCE)
  message("${version} ${url}")
  return()
endif()

foreach(variable IN ITEMS PREFIX WORK_DIR)
  if(NOT ${variable})
    message(FATAL_ERROR "BuildFFmpeg.cmake needs -D${variable}=<path>")
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
  execute_process(
    COMMAND
      "C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe" -latest -products * -requires
      Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    OUTPUT_VARIABLE visual_studio
    OUTPUT_STRIP_TRAILING_WHITESPACE
    COMMAND_ERROR_IS_FATAL ANY
  )
  file(TO_NATIVE_PATH "${visual_studio}/VC/Auxiliary/Build/vcvars64.bat" vcvars)
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
      "@call \"${vcvars}\" >NUL || exit /b 1\r\n"
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
message(STATUS "Building FFmpeg ${version} with ${jobs} jobs")
run_step(build "make -j${jobs} && make install")

# What a package that links FFmpeg statically carries next to it.
file(COPY_FILE "${source}/COPYING.LGPLv2.1" "${PREFIX}/FFmpeg-LICENSE.txt")
file(
  WRITE
  "${PREFIX}/FFmpeg-NOTICE.txt"
  "Tapeloop includes FFmpeg ${version}, linked statically, under the GNU Lesser General\n"
  "Public License version 2.1 or later. The license is in FFmpeg-LICENSE.txt.\n"
  "\n"
  "Source: ${url}\n"
  "SHA-256: ${sha256}\n"
  "\n"
  "Built from that source without changes, configured with:\n"
  "${joined}\n"
)

# Written last: ffmpeg.cmake takes the prefix as complete only once this file is there.
file(WRITE "${PREFIX}/configure-line.txt" "${joined}\n")
file(REMOVE_RECURSE "${WORK_DIR}")
