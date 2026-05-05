# tests/unit/CMakeLists.txt — fast in-memory unit tests.

bist_add_test(test_messages
  SOURCES test_messages.cpp
  LINK bist::ouch)

bist_add_test(test_endian
  SOURCES test_endian.cpp
  LINK bist::core)

bist_add_test(test_ring_buffer
  SOURCES test_ring_buffer.cpp
  LINK bist::core)

bist_add_test(test_codec
  SOURCES test_codec.cpp
  LINK bist::ouch)

bist_add_test(test_soupbintcp
  SOURCES test_soupbintcp.cpp
  LINK bist::net)

bist_add_test(test_throttler
  SOURCES test_throttler.cpp
  LINK bist::domain)

bist_add_test(test_token_registry
  SOURCES test_token_registry.cpp
  LINK bist::domain)

bist_add_test(test_order_book
  SOURCES test_order_book.cpp
  LINK bist::domain)

bist_add_test(test_session_pair
  SOURCES test_session_pair.cpp
  LINK bist::ouch bist::net)

bist_add_test(test_instrument
  SOURCES test_instrument.cpp
  LINK bist::domain)

bist_add_test(test_config
  SOURCES test_config.cpp
  LINK bist::config)

bist_add_test(test_sequence_store
  SOURCES test_sequence_store.cpp
  LINK bist::observability)

bist_add_test(test_replay_matching
  SOURCES test_replay_matching.cpp
  LINK bist::runner bist::fix)

bist_add_test(test_audit_log
  SOURCES test_audit_log.cpp
  LINK bist::observability)

bist_add_test(test_token_encoding
  SOURCES test_token_encoding.cpp
  LINK bist::runner bist::ouch yaml-cpp)

bist_add_test(test_scenario_lint
  SOURCES test_scenario_lint.cpp
  LINK bist::core)
target_compile_definitions(test_scenario_lint PRIVATE
  BIST_SCENARIO_LINT_BIN="$<TARGET_FILE:scenario_lint>")
add_dependencies(test_scenario_lint scenario_lint)

bist_add_test(test_reconnect_policy
  SOURCES test_reconnect_policy.cpp
  LINK bist::core)

bist_add_test(test_epoll_reactor
  SOURCES test_epoll_reactor.cpp
  LINK bist::net)

bist_add_test(test_ptrm
  SOURCES test_ptrm.cpp
  LINK bist::risk)

bist_add_test(test_alloc_guard
  SOURCES test_alloc_guard.cpp alloc_hooks.cpp
  LINK bist::core)
target_compile_definitions(test_alloc_guard PRIVATE BIST_ENABLE_ALLOC_GUARD=1)
