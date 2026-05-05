# src/mock/CMakeLists.txt — bist::mock static library.

add_library(bist_mock STATIC ouch_gateway.cpp)
target_include_directories(bist_mock PUBLIC
  $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
)
target_link_libraries(bist_mock
  PUBLIC bist_core bist_ouch
  PRIVATE bist_warnings
)
target_compile_features(bist_mock PUBLIC cxx_std_20)
add_library(bist::mock ALIAS bist_mock)
