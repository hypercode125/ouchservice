#pragma once
//
// bist/fix/acceptor_mock.hpp — public POD facade for the in-process FIX
// acceptor mock used by `bist_colo --mock` to drive the FIX OE/RD cert
// scenarios offline.
//
// Like bist/fix/facade.hpp, no QuickFIX header is exposed. The Impl lives
// in src/fix/acceptor_mock.cpp and is only built when BIST_BUILD_FIX is on.
// When QuickFIX is disabled the stub returns ErrorCategory::Unsupported.

#include <cstdint>
#include <memory>
#include <string>

#include "bist/core/result.hpp"

namespace bist::fix {

struct AcceptorMockConfig {
  // Initiator-side comp ids (the OE/RD/DC clients). The mock acceptor's
  // SenderCompID will be the initiator's TargetCompID and vice-versa.
  std::string oe_initiator_sender_comp_id;
  std::string oe_initiator_target_comp_id;
  std::string rd_initiator_sender_comp_id;
  std::string rd_initiator_target_comp_id;

  std::string app_data_dictionary;        // path to FIX50SP2.xml
  std::string transport_data_dictionary;  // path to FIXT11.xml
  std::string store_path = "state/fix_mock";
  std::string log_path   = "log/fix_mock";

  int port      = 0;   // 0 → bind ephemeral; resolved port returned
  int heartbeat = 30;
};

class AcceptorMock {
 public:
  static Result<std::unique_ptr<AcceptorMock>> create(AcceptorMockConfig cfg);
  ~AcceptorMock();

  // Returns the bound listening port (after start()).
  [[nodiscard]] int port() const noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  explicit AcceptorMock(std::unique_ptr<Impl> i) noexcept;
};

}  // namespace bist::fix
