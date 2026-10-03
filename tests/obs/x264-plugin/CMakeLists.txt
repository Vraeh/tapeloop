cmake_minimum_required(VERSION 3.28...3.30)

# OBS's obs-x264 plugin, compiled from the OBS sources against an installed libobs.
project(obs-x264 LANGUAGES C)

if(NOT EXISTS "${OBS_SOURCE_DIR}/plugins/obs-x264/obs-x264.c")
  message(FATAL_ERROR "OBS_SOURCE_DIR must point at the OBS sources")
endif()

find_package(libobs REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(X264 REQUIRED IMPORTED_TARGET x264)

add_library(obs-x264 MODULE)
target_sources(
  obs-x264
  PRIVATE
    "${OBS_SOURCE_DIR}/plugins/obs-x264/obs-x264.c"
    "${OBS_SOURCE_DIR}/plugins/obs-x264/obs-x264-plugin-main.c"
    "${OBS_SOURCE_DIR}/shared/opts-parser/opts-parser.c"
)
target_include_directories(obs-x264 PRIVATE "${OBS_SOURCE_DIR}/shared/opts-parser")
target_link_libraries(obs-x264 PRIVATE OBS::libobs PkgConfig::X264)
set_target_properties(obs-x264 PROPERTIES PREFIX "")

install(TARGETS obs-x264 LIBRARY DESTINATION lib/obs-plugins)
install(DIRECTORY "${OBS_SOURCE_DIR}/plugins/obs-x264/data/" DESTINATION share/obs/obs-plugins/obs-x264)
