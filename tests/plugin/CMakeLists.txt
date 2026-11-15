# obs_log from the template the plugin builds it from, with libobs' blogva and the
# allocation of the prefixed format supplied by the tests.
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/../../src/plugin-support.c.in" plugin-support.c @ONLY)
add_library(tapeloop-plugin-support OBJECT ObsLogSeam.c AllocationSeam.h)
target_include_directories(
  tapeloop-plugin-support
  PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../../src"
  PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
)
set_target_properties(tapeloop-plugin-support PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON)
if(NOT MSVC)
  target_compile_options(tapeloop-plugin-support PRIVATE -Wall -Wextra -Wformat -Wformat-security)
endif()

add_executable(tapeloop-plugin-tests ObsLogTests.cpp)
target_link_libraries(tapeloop-plugin-tests PRIVATE tapeloop-plugin-support Catch2::Catch2WithMain)
tapeloop_core_warnings(tapeloop-plugin-tests)
catch_discover_tests(tapeloop-plugin-tests)

# A call to obs_log whose arguments do not match its format must fail to build with the
# format warnings the plugin builds with (cmake/linux/compilerconfig.cmake and
# cmake/common/compiler_common.cmake) as errors. The good calls of the same file build
# with the tests. MSVC is left out: there the declaration only carries
# _Printf_format_string_, which checks nothing outside /analyze, and no build uses it.
if(NOT MSVC)
  function(tapeloop_add_obs_log_format_check target)
    add_library(${target} OBJECT ${ARGN} ObsLogFormat.cpp)
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
    target_compile_options(${target} PRIVATE -Wformat -Wformat-security)
    set_target_properties(${target} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
  endfunction()

  tapeloop_add_obs_log_format_check(obs-log-format-good)
  foreach(call IN ITEMS NONLITERAL_FORMAT WRONG_TYPE MISSING_ARGUMENT)
    string(TOLOWER "${call}" name)
    string(REPLACE "_" "-" name "${name}")
    tapeloop_add_obs_log_format_check(obs-log-format-${name} EXCLUDE_FROM_ALL)
    target_compile_definitions(obs-log-format-${name} PRIVATE TAPELOOP_BAD_${call})
    add_test(
      NAME obs-log-rejects-${name}
      COMMAND "${CMAKE_COMMAND}" --build "${CMAKE_BINARY_DIR}" --target obs-log-format-${name} --config $<CONFIG>
    )
    # Passing on the diagnostic rather than on the failed build keeps a build that fails
    # for another reason, a missing header say, from passing.
    set_tests_properties(
      obs-log-rejects-${name}
      PROPERTIES PASS_REGULAR_EXPRESSION "\\[-Werror[=,](-W)?format" RESOURCE_LOCK tapeloop-build-tree
    )
  endforeach()
endif()
