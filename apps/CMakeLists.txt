# apps/CMakeLists.txt — main binary and auxiliary tools.

# --- Main binary -------------------------------------------------------------

add_executable(bist_colo bist_colo.cpp)
target_link_libraries(bist_colo PRIVATE
  bist::core
  bist::net
  bist::ouch
  bist::domain
  bist::observability
  bist::config
  bist::runner
  bist::mock
  bist::fix
)
target_link_libraries(bist_colo PRIVATE tomlplusplus::tomlplusplus)

set_target_properties(bist_colo PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps
)

# --- Auxiliary tools ---------------------------------------------------------

if(BIST_BUILD_TOOLS)
  add_executable(ouch_decode tools/ouch_decode.cpp)
  target_link_libraries(ouch_decode PRIVATE bist::core bist::ouch)
  set_target_properties(ouch_decode PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/tools
  )

  add_executable(scenario_lint tools/scenario_lint.cpp)
  target_link_libraries(scenario_lint PRIVATE
    bist::core
    bist::runner
    yaml-cpp::yaml-cpp
  )
  set_target_properties(scenario_lint PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/tools
  )

  add_executable(preflight tools/preflight.cpp)
  target_link_libraries(preflight PRIVATE
    bist::core
    bist::net
    bist::config
    tomlplusplus::tomlplusplus
  )
  set_target_properties(preflight PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/tools
  )

  add_executable(latency_bench tools/latency_bench.cpp)
  target_link_libraries(latency_bench PRIVATE
    bist::core
    bist::net
    bist::ouch
    bist::domain
    bist::mock
  )
  set_target_properties(latency_bench PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/apps/tools
  )
endif()
