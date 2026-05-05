#pragma once
//
// bist/core/result.hpp — thin Result<T, Error> alias plus a domain Error
// type. Implementation avoids std::variant / is_nothrow_move_constructible_v
// (both C++17) so the same header can be included from src/fix/, which is
// pinned to C++14 by the QuickFIX bridge.

#include <new>
#include <string>
#include <type_traits>
#include <utility>

namespace bist {

// --- Domain error ------------------------------------------------------------

enum class ErrorCategory {
  Ok,
  Io,
  Protocol,
  Validation,
  Reject,
  Timeout,
  Throttled,
  StateMismatch,
  Unsupported,
};

struct Error {
  ErrorCategory category{ErrorCategory::Ok};
  int           code{0};
  std::string   detail;
};

// --- Result<T> ---------------------------------------------------------------
//
// Tagged union with manual lifetime management. We bypass std::variant
// because it only landed in C++17 and the FIX bridge translation unit
// must compile under C++14.

template <typename T>
class Result {
 public:
  Result(T value) : ok_(true) { new (&value_) T(std::move(value)); }
  Result(Error err) : ok_(false) { new (&error_) Error(std::move(err)); }

  Result(const Result& other) : ok_(other.ok_) {
    if (ok_) new (&value_) T(other.value_);
    else     new (&error_) Error(other.error_);
  }
  Result(Result&& other) noexcept : ok_(other.ok_) {
    if (ok_) new (&value_) T(std::move(other.value_));
    else     new (&error_) Error(std::move(other.error_));
  }
  Result& operator=(const Result& other) {
    if (this != &other) {
      destroy();
      ok_ = other.ok_;
      if (ok_) new (&value_) T(other.value_);
      else     new (&error_) Error(other.error_);
    }
    return *this;
  }
  Result& operator=(Result&& other) noexcept {
    if (this != &other) {
      destroy();
      ok_ = other.ok_;
      if (ok_) new (&value_) T(std::move(other.value_));
      else     new (&error_) Error(std::move(other.error_));
    }
    return *this;
  }
  ~Result() { destroy(); }

  bool ok() const noexcept { return ok_; }
  explicit operator bool() const noexcept { return ok_; }

  T&       value()       &  { return value_; }
  const T& value() const &  { return value_; }
  T        value() &&       { return std::move(value_); }

  const Error& error() const & { return error_; }
  Error        error() &&      { return std::move(error_); }

 private:
  void destroy() {
    if (ok_) value_.~T();
    else     error_.~Error();
  }

  bool ok_;
  union {
    T     value_;
    Error error_;
  };
};

// --- Result<void> specialisation --------------------------------------------

template <>
class Result<void> {
 public:
  Result() = default;
  Result(Error err) : err_(std::move(err)), has_err_(true) {}

  bool ok() const noexcept { return !has_err_; }
  explicit operator bool() const noexcept { return ok(); }

  const Error& error() const & { return err_; }

 private:
  Error err_{};
  bool  has_err_{false};
};

// --- Helpers -----------------------------------------------------------------

inline Error make_error(ErrorCategory cat, std::string detail, int code = 0) {
  Error e;
  e.category = cat;
  e.code     = code;
  e.detail   = std::move(detail);
  return e;
}

}  // namespace bist
