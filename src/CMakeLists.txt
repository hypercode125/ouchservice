# src/CMakeLists.txt
#
# Module layout follows the headers under include/bist/. Each subdirectory
# defines one library target. Targets are linked into the bist_colo binary
# via apps/CMakeLists.txt.
#
# The "core" and "net" targets are intentionally header-only (INTERFACE
# libraries). They expose includes and propagate the warnings interface.
# Heavier modules (ouch, fix, domain, runner, observability) build as static
# OBJECT libraries so that link-time optimisation can fold the entire client
# into a single executable.

# --- core: scalars, time, ring buffer, result ---
add_library(bist_core INTERFACE)
target_include_directories(bist_core INTERFACE
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>
)
target_compile_features(bist_core INTERFACE cxx_std_20)
target_link_libraries(bist_core INTERFACE bist_warnings)
add_library(bist::core ALIAS bist_core)

# --- net: tcp socket, reactor, soupbintcp ---
add_library(bist_net INTERFACE)
target_link_libraries(bist_net INTERFACE bist_core)
add_library(bist::net ALIAS bist_net)

# --- ouch: messages, codec, session, client (currently header-only) ---
add_library(bist_ouch INTERFACE)
target_link_libraries(bist_ouch INTERFACE bist_core bist_net)
add_library(bist::ouch ALIAS bist_ouch)

# --- domain, observability, runner, fix: placeholders for later phases ---
# Implementation will be added incrementally; kept as INTERFACE for now so
# that the build graph stays consistent.

add_library(bist_domain INTERFACE)
target_link_libraries(bist_domain INTERFACE bist_core)
add_library(bist::domain ALIAS bist_domain)

add_library(bist_observability INTERFACE)
target_link_libraries(bist_observability INTERFACE bist_core spdlog::spdlog)
add_library(bist::observability ALIAS bist_observability)

# --- config: TOML loader for endpoints/creds/risk/runner/logging/reactor ---
add_library(bist_config STATIC
  config/config.cpp
)
target_include_directories(bist_config PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(bist_config
  PUBLIC  bist_core
  PRIVATE tomlplusplus::tomlplusplus bist_warnings
)
target_compile_features(bist_config PUBLIC cxx_std_20)
add_library(bist::config ALIAS bist_config)

add_subdirectory(mock)

add_library(bist_runner STATIC
  runner/scenario_loader.cpp
)
target_include_directories(bist_runner PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(bist_runner
  PUBLIC  bist_core bist_domain bist_ouch
  PRIVATE yaml-cpp::yaml-cpp bist_warnings
)
target_compile_features(bist_runner PUBLIC cxx_std_20)
add_library(bist::runner ALIAS bist_runner)

add_subdirectory(fix)

# --- risk: PTRM REST/JWT pre-trade gate ---
add_library(bist_risk STATIC
  risk/ptrm.cpp
)
target_include_directories(bist_risk PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(bist_risk
  PUBLIC  bist_core
  PRIVATE bist_warnings
)
target_compile_features(bist_risk PUBLIC cxx_std_20)
add_library(bist::risk ALIAS bist_risk)
