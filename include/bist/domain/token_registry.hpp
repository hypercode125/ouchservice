#pragma once
//
// bist/domain/token_registry.hpp — generates unique 14-byte OUCH order
// tokens and resolves token → OrderID once the gateway accepts the order.
//
// BIST rejects duplicates with code -800002 ("token is not unique"); we
// avoid the round trip by enforcing uniqueness locally.
//
// Token shape: prefix (configurable, default "C-") + zero-padded sequence,
// truncated/padded to 14 bytes. Sequence wraps after 10^11 - 1, which is
// effectively impossible within a single trading day.

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "bist/core/types.hpp"

namespace bist::domain {

class TokenRegistry {
 public:
  explicit TokenRegistry(std::string prefix = "C-")
      : prefix_(std::move(prefix)) {}

  // Allocate the next sequential token. Throws std::runtime_error only if
  // the tiny formatting buffer overflows, which signals a bug elsewhere.
  [[nodiscard]] OrderToken allocate() {
    char buf[OrderToken::kSize + 1] = {0};
    const std::uint64_t seq = ++sequence_;
    std::snprintf(buf, sizeof(buf), "%s%011llu",
                  prefix_.c_str(),
                  static_cast<unsigned long long>(seq));
    OrderToken tok{std::string_view{buf}};
    seen_.emplace(tok.view());
    return tok;
  }

  // Tag an externally-supplied token (e.g. from a YAML scenario step) as
  // taken. Returns false if the token has already been registered.
  [[nodiscard]] bool register_external(std::string_view tok) {
    auto [it, inserted] = seen_.insert(std::string{tok});
    (void)it;
    return inserted;
  }

  void map_to_order_id(std::string_view tok, OrderId id) {
    token_to_orderid_[std::string{tok}] = id;
  }

  [[nodiscard]] OrderId order_id_of(std::string_view tok) const {
    if (auto it = token_to_orderid_.find(std::string{tok});
        it != token_to_orderid_.end()) {
      return it->second;
    }
    return 0;
  }

  [[nodiscard]] std::size_t size() const noexcept { return seen_.size(); }

 private:
  std::string                                  prefix_;
  std::uint64_t                                sequence_{0};
  std::unordered_set<std::string>              seen_;
  std::unordered_map<std::string, OrderId>     token_to_orderid_;
};

}  // namespace bist::domain
