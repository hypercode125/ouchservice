// tests/unit/alloc_hooks.cpp — global new/delete overrides used by the
// AllocationGuard test only. Compiled into one TU and linked into
// test_alloc_guard so we don't perturb the rest of the test suite.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <new>

#include "bist/core/alloc_guard.hpp"

namespace bist::alloc_internal {
thread_local std::uint64_t allocations_count   = 0;
thread_local std::uint64_t deallocations_count = 0;
thread_local bool          tracking_active     = false;
}

void* operator new(std::size_t n) {
  if (bist::alloc_internal::tracking_active) {
    ++bist::alloc_internal::allocations_count;
  }
  void* p = std::malloc(n == 0 ? 1 : n);
  if (!p) throw std::bad_alloc{};
  return p;
}

void* operator new[](std::size_t n) {
  if (bist::alloc_internal::tracking_active) {
    ++bist::alloc_internal::allocations_count;
  }
  void* p = std::malloc(n == 0 ? 1 : n);
  if (!p) throw std::bad_alloc{};
  return p;
}

void operator delete(void* p) noexcept {
  if (bist::alloc_internal::tracking_active && p) {
    ++bist::alloc_internal::deallocations_count;
  }
  std::free(p);
}

void operator delete[](void* p) noexcept {
  if (bist::alloc_internal::tracking_active && p) {
    ++bist::alloc_internal::deallocations_count;
  }
  std::free(p);
}

void operator delete(void* p, std::size_t) noexcept {
  if (bist::alloc_internal::tracking_active && p) {
    ++bist::alloc_internal::deallocations_count;
  }
  std::free(p);
}

void operator delete[](void* p, std::size_t) noexcept {
  if (bist::alloc_internal::tracking_active && p) {
    ++bist::alloc_internal::deallocations_count;
  }
  std::free(p);
}
