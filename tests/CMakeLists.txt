# tests/CMakeLists.txt
#
# GoogleTest-based unit and integration tests. Each test lives under one of
# the three subdirectories and is registered with CTest via the helper.

include(GoogleTest)

function(bist_add_test name)
  cmake_parse_arguments(arg "" "" "SOURCES;LINK" ${ARGN})
  add_executable(${name} ${arg_SOURCES})
  target_link_libraries(${name} PRIVATE
    GTest::gtest_main
    bist::core
    ${arg_LINK}
  )
  set_target_properties(${name} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/tests
  )
  gtest_discover_tests(${name})
endfunction()

add_subdirectory(unit)
# add_subdirectory(integration)   # phase 2
# add_subdirectory(cert_replay)   # phase 3
