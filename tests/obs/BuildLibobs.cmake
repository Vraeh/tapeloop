# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) 2026 Vicente Aedo <ryde1337@gmail.com>

# Builds the libobs the glue tests run against, from the OBS sources pinned in
# buildspec.json, without the frontend, scripting or the plugin set, and installs it
# with libobs-opengl and the obs-x264 plugin into a prefix. libobs finds its data
# through the prefix it was built for, so the prefix cannot be moved afterwards.
#
#   cmake -DPREFIX=<dir> -DWORK_DIR=<dir> [-DBUILD_TYPE=<type>] [-DSANITIZE=ON]
#         [-DEXTRA_C_FLAGS=<flags>] -P tests/obs/BuildLibobs.cmake
#
# SANITIZE builds libobs and the plugin with ASan and UBSan, to match a sanitized test
# binary. EXTRA_C_FLAGS reaches the C and C++ compilers of both builds.

cmake_minimum_required(VERSION 3.28)

foreach(required IN ITEMS PREFIX WORK_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()
if(NOT DEFINED BUILD_TYPE)
  set(BUILD_TYPE RelWithDebInfo)
endif()

cmake_path(ABSOLUTE_PATH PREFIX NORMALIZE)
cmake_path(ABSOLUTE_PATH WORK_DIR NORMALIZE)
cmake_path(GET CMAKE_CURRENT_LIST_DIR PARENT_PATH tests_dir)
cmake_path(GET tests_dir PARENT_PATH repo_dir)

file(READ "${repo_dir}/buildspec.json" buildspec)
string(JSON version GET "${buildspec}" dependencies obs-studio version)
string(JSON base_url GET "${buildspec}" dependencies obs-studio baseUrl)
# buildspec.json lists the tarball under each platform the plugin ships for; it is the
# same file everywhere.
string(JSON hash GET "${buildspec}" dependencies obs-studio hashes windows-x64)

set(archive "${WORK_DIR}/obs-studio-${version}.tar.gz")
set(source_dir "${WORK_DIR}/obs-studio-${version}")
file(DOWNLOAD "${base_url}/${version}.tar.gz" "${archive}" EXPECTED_HASH SHA256=${hash} STATUS download_status)
list(GET download_status 0 download_code)
if(NOT download_code EQUAL 0)
  message(FATAL_ERROR "Downloading the OBS sources failed: ${download_status}")
endif()
# The marker is written last, so an extraction cut short is redone, and a tree from
# another archive is replaced.
set(marker "${source_dir}/.extracted-${hash}")
if(NOT EXISTS "${marker}")
  file(REMOVE_RECURSE "${source_dir}")
  file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${WORK_DIR}")
  file(TOUCH "${marker}")
endif()

set(c_flags "${EXTRA_C_FLAGS}")
set(linker_flags "")
if(SANITIZE)
  string(APPEND c_flags " -fsanitize=address,undefined -fno-omit-frame-pointer")
  set(linker_flags "-fsanitize=address,undefined")
endif()

function(run_step description)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE result COMMAND_ECHO STDOUT)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "${description} failed")
  endif()
endfunction()

# Wayland stays on, as in OBS's own Linux builds: with it off, the OpenGL loader is
# built without its EGL half on CMake 3, while the X11 backend needs it.
set(obs_build "${WORK_DIR}/build-obs")
run_step(
  "Configuring OBS"
  "${CMAKE_COMMAND}" -S "${source_dir}" -B "${obs_build}" -G Ninja --fresh
  "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
  "-DCMAKE_INSTALL_PREFIX=${PREFIX}"
  "-DCMAKE_C_FLAGS=${c_flags}"
  "-DCMAKE_CXX_FLAGS=${c_flags}"
  "-DCMAKE_EXE_LINKER_FLAGS=${linker_flags}"
  "-DCMAKE_SHARED_LINKER_FLAGS=${linker_flags}"
  "-DCMAKE_MODULE_LINKER_FLAGS=${linker_flags}"
  "-DOBS_VERSION_OVERRIDE=${version}"
  -DENABLE_UI=OFF
  -DENABLE_FRONTEND=OFF
  -DENABLE_SCRIPTING=OFF
  -DENABLE_PLUGINS=OFF
  -DENABLE_PULSEAUDIO=OFF
)
run_step("Building libobs" "${CMAKE_COMMAND}" --build "${obs_build}")
run_step("Installing libobs" "${CMAKE_COMMAND}" --install "${obs_build}")

# The plugin set cannot be configured from the tarball, which lacks the obs-browser and
# obs-websocket submodules, so obs-x264 is built on its own against the installed libobs.
set(x264_build "${WORK_DIR}/build-obs-x264")
run_step(
  "Configuring obs-x264"
  "${CMAKE_COMMAND}" -S "${CMAKE_CURRENT_LIST_DIR}/x264-plugin" -B "${x264_build}" -G Ninja --fresh
  "-DCMAKE_BUILD_TYPE=${BUILD_TYPE}"
  "-DCMAKE_INSTALL_PREFIX=${PREFIX}"
  "-DCMAKE_PREFIX_PATH=${PREFIX}"
  "-DCMAKE_C_FLAGS=${c_flags}"
  "-DCMAKE_MODULE_LINKER_FLAGS=${linker_flags}"
  "-DOBS_SOURCE_DIR=${source_dir}"
)
run_step("Building obs-x264" "${CMAKE_COMMAND}" --build "${x264_build}")
run_step("Installing obs-x264" "${CMAKE_COMMAND}" --install "${x264_build}")
