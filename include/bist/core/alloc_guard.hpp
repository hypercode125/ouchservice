#pragma once
//
// bist/core/alloc_guard.hpp — RAII assertion that no heap allocation
// happens inside the guarded scope.
//
// Production hot path holds no allocations beyond the bounded SPSC ring
// slots (CLAUDE.md). This guard exists so we can prove that property in
// Debug + sanitizer builds without paying any cost in Release.
//
// Mechanism: replaces the global new/delete operators with hooks that
// bump a TLS counter; AllocationGuard captures the counter on entry and
// triggers `assertion_failure_` on destruction if it changed. In Release
// builds the guard is a no-op so colo binaries do not pay any overhead.
//
// Usage:
//   void hot_thread_callback() {
//     bist::AllocationGuard g;          // Debug-only assertion
//     ...                                // hot path; must not allocate
//   }
//
// To wire the global operator overrides, link the matching .cpp into the
// test binary (see tests/unit/test_alloc_guard.cpp). Production binaries
// do NOT link the overrides — the guard becomes inert.

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace bist {

namespace alloc_internal {
// TLS allocation counter mutated by the global operator new/delete
// overrides in alloc_hooks.cpp (linked only into Debug test targets).
extern thread_local std::uint64_t allocations_count;
extern thread_local std::uint64_t deallocations_count;
// Global enable/disable flag — when false, the override defers to the
// system allocator without bumping counters. AllocationGuard flips this
// during the guarded scope so unrelated TUs are unaffected.
extern thread_local bool          tracking_active;
}  // namespace alloc_internal

class AllocationGuard {
 public:
#if defined(BIST_ENABLE_ALLOC_GUARD)
  AllocationGuard() noexcept
      : start_alloc_(alloc_internal::allocations_count),
        start_dealloc_(alloc_internal::deallocations_count) {
    alloc_internal::tracking_active = true;
  }
  ~AllocationGuard() noexcept(false) {
    alloc_internal::tracking_active = false;
    const std::uint64_t allocs = alloc_internal::allocations_count - start_alloc_;
    if (allocs != 0) {
      // Fail loud — caller violated the no-allocation contract.
      std::abort();
    }
  }
  AllocationGuard(const AllocationGuard&)            = delete;
  AllocationGuard& operator=(const AllocationGuard&) = delete;

  std::uint64_t observed_allocations() const noexcept {
    return alloc_internal::allocations_count - start_alloc_;
  }
  std::uint64_t observed_deallocations() const noexcept {
    return alloc_internal::deallocations_count - start_dealloc_;
  }

 private:
  std::uint64_t start_alloc_;
  std::uint64_t start_dealloc_;
#else
  AllocationGuard() noexcept = default;
  ~AllocationGuard() = default;
  std::uint64_t observed_allocations() const noexcept { return 0; }
  std::uint64_t observed_deallocations() const noexcept { return 0; }
#endif
};

}  // namespace bist
