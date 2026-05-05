#pragma once
//
// bist/core/endian.hpp — typed big-endian scalar wrappers.
//
// OUCH messages encode every numeric field as big-endian. Wrapping these as
// distinct types pinned to the wire encoding eliminates an entire class of
// "did I forget to byteswap?" bugs at compile time.

#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace bist {

// --- Generic big-endian POD wrapper -----------------------------------------
//
// Storage is always sizeof(T) raw bytes in network order. Implicit conversions
// are deliberately avoided: callers must opt in via .get()/.set() so that
// wire-level operations stay visible at the call site.

template <typename T>
class BigEndian {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>,
                "BigEndian only supports integral types other than bool");
  static_assert(sizeof(T) == 1 || sizeof(T) == 2 ||
                sizeof(T) == 4 || sizeof(T) == 8,
                "BigEndian only supports 1/2/4/8-byte integers");

 public:
  using value_type = T;

  BigEndian() noexcept { std::memset(bytes_, 0, sizeof(bytes_)); }
  explicit BigEndian(T v) noexcept { set(v); }

  [[nodiscard]] T get() const noexcept {
    if constexpr (sizeof(T) == 1) {
      return static_cast<T>(bytes_[0]);
    } else {
      T v;
      std::memcpy(&v, bytes_, sizeof(T));
      if constexpr (std::endian::native == std::endian::little) {
        v = byteswap(v);
      }
      return v;
    }
  }

  void set(T v) noexcept {
    if constexpr (sizeof(T) == 1) {
      bytes_[0] = static_cast<std::uint8_t>(v);
    } else {
      if constexpr (std::endian::native == std::endian::little) {
        v = byteswap(v);
      }
      std::memcpy(bytes_, &v, sizeof(T));
    }
  }

  [[nodiscard]] const std::uint8_t* data() const noexcept { return bytes_; }
  [[nodiscard]] std::uint8_t*       data()       noexcept { return bytes_; }

  friend bool operator==(const BigEndian&, const BigEndian&) = default;

 private:
  static T byteswap(T v) noexcept {
    if constexpr (sizeof(T) == 2) {
      return static_cast<T>(__builtin_bswap16(static_cast<std::uint16_t>(v)));
    } else if constexpr (sizeof(T) == 4) {
      return static_cast<T>(__builtin_bswap32(static_cast<std::uint32_t>(v)));
    } else {  // 8
      return static_cast<T>(__builtin_bswap64(static_cast<std::uint64_t>(v)));
    }
  }

  std::uint8_t bytes_[sizeof(T)];
};

using be_u8  = BigEndian<std::uint8_t>;
using be_u16 = BigEndian<std::uint16_t>;
using be_u32 = BigEndian<std::uint32_t>;
using be_u64 = BigEndian<std::uint64_t>;
using be_i32 = BigEndian<std::int32_t>;
using be_i64 = BigEndian<std::int64_t>;

static_assert(sizeof(be_u8)  == 1);
static_assert(sizeof(be_u16) == 2);
static_assert(sizeof(be_u32) == 4);
static_assert(sizeof(be_u64) == 8);
static_assert(sizeof(be_i32) == 4);
static_assert(sizeof(be_i64) == 8);

}  // namespace bist
