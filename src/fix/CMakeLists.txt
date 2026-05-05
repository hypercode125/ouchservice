# src/fix/CMakeLists.txt — bist::fix static library.
#
# Two translation units share the public POD facade in
# include/bist/fix/facade.hpp:
#
#   facade_stub.cpp        - always built. C++20-clean. When QuickFIX is
#                            disabled the stub returns
#                            ErrorCategory::Unsupported from every method.
#                            When QuickFIX is enabled the stub's TU body
#                            is fully `#if !defined(BIST_HAS_QUICKFIX)`
#                            and contributes no symbols, so the linker
#                            picks the real implementations from
#                            facade_quickfix.cpp.
#
#   facade_quickfix.cpp    - only built when BIST_BUILD_FIX is on. Pinned
#                            to -std=gnu++14 because QuickFIX 1.15.1 still
#                            uses std::auto_ptr (removed C++17) and
#                            dynamic exception specifications (removed
#                            C++17). The fence keeps the rest of the
#                            project on C++20.

add_library(bist_fix STATIC facade_stub.cpp)
target_include_directories(bist_fix PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(bist_fix
  PUBLIC  bist_core bist_domain
  PRIVATE bist_warnings
)
target_compile_features(bist_fix PUBLIC cxx_std_20)
add_library(bist::fix ALIAS bist_fix)

if(BIST_ENABLE_QUICKFIX AND BIST_BUILD_FIX)
  target_sources(bist_fix PRIVATE facade_quickfix.cpp acceptor_mock.cpp)
  target_link_libraries(bist_fix PRIVATE quickfix)
  target_compile_definitions(bist_fix PUBLIC
    BIST_HAS_QUICKFIX=1
    BIST_FIX_DICT_DIR="${CMAKE_BINARY_DIR}/_deps/quickfix-src/spec"
  )

  # The whole-project warn-wall (bist_warnings) declares cxx_std_20
  # INTERFACE which CMake auto-escalates onto our consumer. We override
  # for QuickFIX-touching TUs only by stamping `-std=gnu++14` at the end
  # of their compile line, plus disabling the warnings QuickFIX headers
  # trip.
  set_source_files_properties(facade_quickfix.cpp acceptor_mock.cpp PROPERTIES
    COMPILE_OPTIONS "-std=gnu++14;-Wno-deprecated-declarations;-Wno-deprecated-dynamic-exception-spec;-Wno-shadow;-Wno-old-style-cast;-Wno-conversion;-Wno-sign-conversion;-Wno-double-promotion"
  )
  # Pull QuickFIX's PRIVATE include dirs onto our target. The top-level
  # CMakeLists has already mirrored src/C++ into a quickfix-include
  # subdirectory.
  target_include_directories(bist_fix SYSTEM PRIVATE
    ${CMAKE_BINARY_DIR}/quickfix-include
    ${CMAKE_BINARY_DIR}/_deps/quickfix-src/src/C++
    ${PROJECT_SOURCE_DIR}
  )
endif()
