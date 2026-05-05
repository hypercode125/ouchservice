cmake_minimum_required(VERSION 3.25)

project(bist_colo_client
  VERSION 0.1.0
  DESCRIPTION "BIST Pay Piyasası kolokasyon trading client (OUCH + FIX OE/RD)"
  LANGUAGES CXX
)

# ---------- Build options ----------
option(BIST_BUILD_TESTS        "Build unit and integration tests"      ON)
option(BIST_BUILD_TOOLS        "Build auxiliary tools (ouch_decode, scenario_lint)" ON)
option(BIST_ENABLE_SANITIZERS  "Enable AddressSanitizer + UBSan in Debug"   OFF)
option(BIST_ENABLE_TSAN        "Enable ThreadSanitizer (separate config)"   OFF)
option(BIST_ENABLE_LTO         "Link-time optimization in Release"           ON)
option(BIST_ENABLE_QUICKFIX    "Pull and link QuickFIX (FIX engine)"        OFF)
option(BIST_BUILD_FIX          "Compile bist::fix (requires QuickFIX)"      OFF)
option(BIST_NATIVE_ARCH        "Compile with -march=native (colo target)"    OFF)

# ---------- Language ----------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

# ---------- Warnings + flags ----------
add_library(bist_warnings INTERFACE)
target_compile_options(bist_warnings INTERFACE
  -Wall -Wextra -Wpedantic
  -Wshadow -Wnon-virtual-dtor -Wold-style-cast
  -Wcast-align -Wunused -Woverloaded-virtual
  -Wconversion -Wsign-conversion
  -Wnull-dereference -Wdouble-promotion
  -Wformat=2 -Wimplicit-fallthrough
)

if(BIST_NATIVE_ARCH)
  target_compile_options(bist_warnings INTERFACE -march=native)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release" AND BIST_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT ipo_ok OUTPUT ipo_err)
  if(ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
  else()
    message(WARNING "IPO/LTO not supported: ${ipo_err}")
  endif()
endif()

if(BIST_ENABLE_SANITIZERS AND CMAKE_BUILD_TYPE STREQUAL "Debug")
  target_compile_options(bist_warnings INTERFACE
    -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(bist_warnings INTERFACE
    -fsanitize=address,undefined)
endif()

if(BIST_ENABLE_TSAN)
  target_compile_options(bist_warnings INTERFACE
    -fsanitize=thread -fno-omit-frame-pointer)
  target_link_options(bist_warnings INTERFACE -fsanitize=thread)
endif()

# ---------- Dependencies (FetchContent) ----------
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# spdlog — async logger
FetchContent_Declare(spdlog
  GIT_REPOSITORY https://github.com/gabime/spdlog.git
  GIT_TAG v1.14.1
  GIT_SHALLOW TRUE)
set(SPDLOG_BUILD_SHARED OFF CACHE BOOL "" FORCE)
set(SPDLOG_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(spdlog)

# tomlplusplus — config
FetchContent_Declare(tomlplusplus
  GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git
  GIT_TAG v3.4.0
  GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(tomlplusplus)

# yaml-cpp — scenario runner
# 0.8.0 still uses cmake_minimum_required(3.0) which CMake 4.x rejects.
# We bridge by setting CMAKE_POLICY_VERSION_MINIMUM around the subdirectory
# add. This is a localized override; main project policies are untouched.
FetchContent_Declare(yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
  GIT_SHALLOW TRUE)
set(YAML_CPP_BUILD_TESTS  OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS  OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_CONTRIB OFF CACHE BOOL "" FORCE)
set(YAML_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
FetchContent_MakeAvailable(yaml-cpp)
unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
# Mark yaml-cpp headers as SYSTEM so internal -Wshadow doesn't pollute our
# compile output. yaml-cpp::yaml-cpp is an ALIAS, so we modify the real
# target name (yaml-cpp).
if(TARGET yaml-cpp)
  get_target_property(_yaml_inc yaml-cpp INTERFACE_INCLUDE_DIRECTORIES)
  if(_yaml_inc)
    set_target_properties(yaml-cpp PROPERTIES
      INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${_yaml_inc}")
  endif()
endif()

# QuickFIX/C++ — FIX engine (optional). 1.15.1 ships
# cmake_minimum_required(3.0); CMake 4.x rejects that without an explicit
# policy bridge. Mark its include dirs SYSTEM after the populate so internal
# warnings don't leak into our compile output.
if(BIST_ENABLE_QUICKFIX)
  FetchContent_Declare(quickfix
    GIT_REPOSITORY https://github.com/quickfix/quickfix.git
    GIT_TAG v1.15.1
    GIT_SHALLOW TRUE)
  set(HAVE_SSL          OFF CACHE BOOL "" FORCE)
  set(QUICKFIX_TESTS    OFF CACHE BOOL "" FORCE)
  set(QUICKFIX_EXAMPLES OFF CACHE BOOL "" FORCE)
  set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
  # QuickFIX expects its own cmake/ helpers on CMAKE_MODULE_PATH. The
  # MakeAvailable call below issues add_subdirectory; QuickFIX's top-level
  # CMakeLists references CMAKE_SOURCE_DIR (our root) instead of
  # CMAKE_CURRENT_SOURCE_DIR (its own root) when prepending its module
  # path, which breaks include(FindSharedPtr). Pre-populate the path here.
  FetchContent_GetProperties(quickfix)
  if(NOT quickfix_POPULATED)
    FetchContent_Populate(quickfix)
  endif()
  list(APPEND CMAKE_MODULE_PATH "${quickfix_SOURCE_DIR}/cmake")

  # QuickfixPlatformSettings.cmake assumes its own source dir is
  # ${CMAKE_SOURCE_DIR}; under FetchContent that's our project root, so
  # the auto-generated Allocator.h / config.h end up in the wrong place
  # (or fail silently). Pre-create them at the right location before the
  # subdirectory pulls them in. We also pre-create the include/quickfix
  # symlink that QuickfixPrebuildSetup expects.
  if(NOT EXISTS "${quickfix_SOURCE_DIR}/src/C++/Allocator.h")
    file(WRITE "${quickfix_SOURCE_DIR}/src/C++/Allocator.h"
         "/* Empty file generated by bist-colo CMakeLists pre-bridge */\n")
  endif()
  # AtomicCount.h ships with hand-rolled x86 `lock xadd` inline asm under
  # the GCC branch. On arm64 (Apple Silicon and Linux ARM) that fails to
  # assemble. Replace the file with a portable std::atomic-backed
  # implementation that satisfies QuickFIX's `atomic_count` API.
  file(WRITE "${quickfix_SOURCE_DIR}/src/C++/AtomicCount.h"
"/* Replaced by bist-colo CMakeLists: portable std::atomic version. */\n"
"#ifndef ATOMIC_COUNT\n"
"#define ATOMIC_COUNT\n"
"#include <atomic>\n"
"namespace FIX {\n"
"class atomic_count {\n"
" public:\n"
"  explicit atomic_count(long v = 0) : value_(v) {}\n"
"  long operator++() { return ++value_; }\n"
"  long operator--() { return --value_; }\n"
"  operator long() const { return value_.load(); }\n"
" private:\n"
"  std::atomic<long> value_;\n"
"};\n"
"}\n"
"#endif\n")
  if(NOT EXISTS "${quickfix_SOURCE_DIR}/config.h")
    file(WRITE "${quickfix_SOURCE_DIR}/config.h"
         "/* Empty file generated by bist-colo CMakeLists pre-bridge */\n")
  endif()
  file(MAKE_DIRECTORY "${quickfix_SOURCE_DIR}/include")
  if(NOT EXISTS "${quickfix_SOURCE_DIR}/include/quickfix")
    if(WIN32)
      execute_process(COMMAND ${CMAKE_COMMAND} -E copy_directory
                      "${quickfix_SOURCE_DIR}/src/C++"
                      "${quickfix_SOURCE_DIR}/include/quickfix")
    else()
      execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
                      "${quickfix_SOURCE_DIR}/src/C++"
                      "${quickfix_SOURCE_DIR}/include/quickfix")
    endif()
  endif()

  # FetchContent_Populate above already grabbed the source. Calling
  # FetchContent_MakeAvailable here would no-op the add_subdirectory since
  # POPULATED is already set. Add the subdir manually so QuickFIX joins the
  # build graph and produces libquickfix.{so,dylib}.
  if(NOT TARGET quickfix)
    add_subdirectory(${quickfix_SOURCE_DIR} ${quickfix_BINARY_DIR}
                     EXCLUDE_FROM_ALL)
  endif()
  unset(CMAKE_POLICY_VERSION_MINIMUM CACHE)
  # QuickFIX ships its headers directly under src/C++/ but every internal
  # and consumer-facing include path uses "quickfix/Message.h" form.
  # Recreate that layout under <binary>/quickfix-include/quickfix as a
  # symlink at configure time, then surface it as a SYSTEM include on the
  # quickfix target so our consumer (bist_fix) can compile.
  set(_qf_layout "${CMAKE_BINARY_DIR}/quickfix-include")
  file(MAKE_DIRECTORY "${_qf_layout}")
  if(NOT EXISTS "${_qf_layout}/quickfix")
    if(WIN32)
      execute_process(COMMAND ${CMAKE_COMMAND} -E copy_directory
                      "${quickfix_SOURCE_DIR}/src/C++"
                      "${_qf_layout}/quickfix")
    else()
      execute_process(COMMAND ${CMAKE_COMMAND} -E create_symlink
                      "${quickfix_SOURCE_DIR}/src/C++"
                      "${_qf_layout}/quickfix")
    endif()
  endif()
  if(TARGET quickfix)
    target_include_directories(quickfix SYSTEM PUBLIC
      "${_qf_layout}"
      "${quickfix_SOURCE_DIR}/src/C++")
    # QuickFIX 1.15.1 source itself must build at C++14 because of the same
    # dynamic-exception-spec / std::auto_ptr usage we work around in our
    # own bist_fix TU. Override the standard on its target so CMake stops
    # promoting it to C++20 when our INTERFACE warn-wall propagates.
    set_target_properties(quickfix PROPERTIES
      CXX_STANDARD 14
      CXX_STANDARD_REQUIRED ON
      CXX_EXTENSIONS ON)
    target_compile_options(quickfix PRIVATE
      -std=gnu++14
      -Wno-deprecated-declarations
      -Wno-deprecated-dynamic-exception-spec
      -Wno-shadow
      -Wno-old-style-cast
      -Wno-conversion
      -Wno-sign-conversion
      -Wno-double-promotion
      -Wno-unused
      -Wno-pedantic
      -Wno-format
      -Wno-implicit-fallthrough)
  endif()
endif()

# GoogleTest
if(BIST_BUILD_TESTS)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.14.0
    GIT_SHALLOW TRUE)
  set(BUILD_GMOCK ON  CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  enable_testing()
endif()

# ---------- Library targets ----------
add_subdirectory(src)

if(BIST_BUILD_TESTS)
  add_subdirectory(tests)
endif()

add_subdirectory(apps)

# ---------- Install ----------
include(GNUInstallDirs)
install(DIRECTORY include/bist
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  FILES_MATCHING PATTERN "*.hpp")

# ---------- Status ----------
message(STATUS "")
message(STATUS "bist-colo-client ${PROJECT_VERSION}")
message(STATUS "  Build type           : ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler             : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "  Tests                : ${BIST_BUILD_TESTS}")
message(STATUS "  Tools                : ${BIST_BUILD_TOOLS}")
message(STATUS "  Sanitizers (ASan+UB) : ${BIST_ENABLE_SANITIZERS}")
message(STATUS "  TSan                 : ${BIST_ENABLE_TSAN}")
message(STATUS "  LTO                  : ${BIST_ENABLE_LTO}")
message(STATUS "  QuickFIX             : ${BIST_ENABLE_QUICKFIX}")
message(STATUS "  -march=native        : ${BIST_NATIVE_ARCH}")
message(STATUS "")
