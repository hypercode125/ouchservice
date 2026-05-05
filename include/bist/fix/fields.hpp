#pragma once
//
// bist/fix/fields.hpp — BIST FIX 5.0 SP2 field tags and value constants.
//
// QuickFIX ships the standard FIX 5.0 SP2 data dictionary with every tag
// the cert program touches. BIST layers a small set of custom values on
// top — primarily party-role codes that carry the Agency/Fund Code (AFK)
// attribution required by the Pay Piyasası rules. We expose those here as
// strongly-typed constants so call sites read like the BIST documentation
// rather than as anonymous tag/value pairs.
//
// Numeric tags follow the FIX 5.0 SP2 spec; AFK constants follow the
// Pay Piyasası FIX Sertifikasyon Programı (Kasım 2025).

#include <cstdint>
#if __cplusplus >= 201703L
#include <string_view>
#endif

namespace bist::fix {

// --- Tags --------------------------------------------------------------------

namespace tag {
// FIX 5.0 SP2 standard fields used by the BIST cert.
inline constexpr std::int32_t Account            = 1;
inline constexpr std::int32_t ClOrdID            = 11;
inline constexpr std::int32_t HandlInst          = 21;
inline constexpr std::int32_t SecurityIDSource   = 22;
inline constexpr std::int32_t MsgType            = 35;
inline constexpr std::int32_t OrderID            = 37;
inline constexpr std::int32_t OrderQty           = 38;
inline constexpr std::int32_t OrdStatus          = 39;
inline constexpr std::int32_t OrdType            = 40;
inline constexpr std::int32_t OrigClOrdID        = 41;
inline constexpr std::int32_t Price              = 44;
inline constexpr std::int32_t SecurityID         = 48;
inline constexpr std::int32_t Side               = 54;
inline constexpr std::int32_t Symbol             = 55;
inline constexpr std::int32_t TimeInForce        = 59;
inline constexpr std::int32_t TransactTime       = 60;
inline constexpr std::int32_t SettlmntTyp        = 63;
inline constexpr std::int32_t ExecType           = 150;
inline constexpr std::int32_t LeavesQty          = 151;
inline constexpr std::int32_t MaxFloor           = 111;   // iceberg display qty
inline constexpr std::int32_t DisplayQty         = 1138;
inline constexpr std::int32_t NoPartyIDs         = 453;
inline constexpr std::int32_t PartyID            = 448;
inline constexpr std::int32_t PartyIDSource      = 447;
inline constexpr std::int32_t PartyRole          = 452;

// Logon-time password rotation.
inline constexpr std::int32_t Username           = 553;
inline constexpr std::int32_t Password           = 554;
inline constexpr std::int32_t NewPassword        = 925;
inline constexpr std::int32_t SessionStatus      = 1409;

// Reference Data subscription.
inline constexpr std::int32_t ApplReqID          = 1346;   // ApplicationMessageRequest
inline constexpr std::int32_t ApplReqType        = 1347;
inline constexpr std::int32_t ApplResponseError  = 1354;

// Trade Capture Report.
inline constexpr std::int32_t TradeReportID      = 571;
inline constexpr std::int32_t TradeReportRefID   = 572;
inline constexpr std::int32_t TrdType            = 828;
}  // namespace tag

// --- Values ------------------------------------------------------------------

// FIX Side(54).  Note these are FIX, not OUCH; OUCH uses 'B'/'S'/'T'.
namespace side {
inline constexpr char Buy        = '1';
inline constexpr char Sell       = '2';
inline constexpr char SellShort  = '5';
}  // namespace side

// FIX OrdType(40).
namespace ord_type {
inline constexpr char Market           = '1';
inline constexpr char Limit            = '2';
inline constexpr char Stop             = '3';
inline constexpr char StopLimit        = '4';
inline constexpr char MarketWithLeftover = 'K';   // BIST "Market to Limit"
inline constexpr char Imbalance        = 'M';     // BIST imbalance order
inline constexpr char MidpointLimit    = 'P';
inline constexpr char MidpointMarket   = 'Q';
}  // namespace ord_type

// FIX TimeInForce(59).
namespace tif {
inline constexpr char Day              = '0';
inline constexpr char GoodTillCancel   = '1';
inline constexpr char ImmediateOrCancel = '3';   // FaK
inline constexpr char FillOrKill       = '4';
}  // namespace tif

// FIX SessionStatus(1409). Carried back in Logon response.
namespace session_status {
inline constexpr int Active                  = 0;
inline constexpr int SessionPasswordChanged  = 1;
inline constexpr int SessionPasswordExpiring = 2;
inline constexpr int NewSessionPasswordDoesNotComply = 3;
inline constexpr int LogoutComplete          = 4;
inline constexpr int InvalidUsernameOrPassword = 5;
inline constexpr int AccountLocked           = 6;
inline constexpr int LogonsNotAllowedAtThisTime = 7;
inline constexpr int PasswordExpired         = 8;
}  // namespace session_status

// FIX PartyRole(452) values used by BIST. The values flagged "BIST" extend
// the standard FIX taxonomy.
namespace party_role {
inline constexpr int ExecutingFirm           = 1;
inline constexpr int OrderOriginationTrader  = 11;
// BIST: 76 carries the Agency/Fund Code (AFK).
inline constexpr int DeskID                  = 76;
}  // namespace party_role

// FIX PartyIDSource(447) — when carrying AFK we use 'D' (proprietary code).
namespace party_id_source {
inline constexpr char Proprietary = 'D';
}  // namespace party_id_source

// AFK literal codes (PartyID value when PartyRole=DeskID). Plain const
// char* keeps this header consumable from both C++14 (bist_fix) and C++20
// (rest of the project) translation units.
namespace afk {
inline constexpr const char* Fund      = "XRM";
inline constexpr const char* MmClient  = "PYM";
inline constexpr const char* MmHouse   = "PYP";
}  // namespace afk

// FIX ApplReqType(1347) values used by RD subscription.
namespace appl_req_type {
inline constexpr int Subscribe              = 0;
inline constexpr int Unsubscribe            = 1;
inline constexpr int RetransmitMessages     = 2;
inline constexpr int RequestAndSubscribe    = 3;
}  // namespace appl_req_type

}  // namespace bist::fix
