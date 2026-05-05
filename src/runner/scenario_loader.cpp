// src/runner/scenario_loader.cpp — YAML → Scenario translation.
//
// We intentionally accept lax types: numeric ids, prices, tokens may all
// arrive as strings or numbers. Everything but the "expect.fields" map is
// strongly typed; expect.fields stays as <string,string> so that callers
// can compare equally regardless of how the YAML author wrote the value.

#include "bist/runner/scenario.hpp"

#include <stdexcept>
#include <string>

#include <yaml-cpp/yaml.h>

namespace bist::runner {

namespace {

template <typename T>
std::optional<T> opt_as(const YAML::Node& n) {
  if (!n) return std::nullopt;
  try { return n.as<T>(); } catch (...) { return std::nullopt; }
}

std::string scalar(const YAML::Node& n, const std::string& fallback = {}) {
  if (!n) return fallback;
  if (n.IsScalar()) return n.as<std::string>();
  return fallback;
}

Side side_from(const std::string& s) {
  if (s == "BUY"  || s == "B" || s == "buy")  return Side::Buy;
  if (s == "SELL" || s == "S" || s == "sell") return Side::Sell;
  if (s == "SHORT" || s == "T")               return Side::ShortSell;
  return Side::Buy;
}

TimeInForce tif_from(const std::string& s) {
  if (s == "DAY" || s == "Day" || s == "day") return TimeInForce::Day;
  if (s == "IOC" || s == "FAK") return TimeInForce::ImmediateOrCancel;
  if (s == "FOK") return TimeInForce::FillOrKill;
  return TimeInForce::Day;
}

ClientCategory category_from(const std::string& s) {
  if (s == "CLIENT")  return ClientCategory::Client;
  if (s == "HOUSE")   return ClientCategory::House;
  if (s == "FUND")    return ClientCategory::Fund;
  if (s == "PORTFOLIO" || s == "MGMT") return ClientCategory::PortfolioMgmt;
  return ClientCategory::Client;
}

Expect parse_expect(const YAML::Node& n) {
  Expect e{};
  if (n.IsScalar()) {
    e.msg = n.as<std::string>();
    return e;
  }
  if (!n.IsMap()) return e;
  e.msg         = scalar(n["msg"]);
  if (auto occ = opt_as<int>(n["occurrences"])) e.occurrences = *occ;
  if (auto t   = opt_as<int>(n["timeout_ms"]))  e.timeout_ms  = *t;
  if (n["fields"] && n["fields"].IsMap()) {
    for (const auto& kv : n["fields"]) {
      e.fields[kv.first.as<std::string>()] =
          kv.second.IsScalar() ? kv.second.as<std::string>() : "";
    }
  }
  return e;
}

LoginArgs parse_login_args(const YAML::Node& a) {
  LoginArgs out{};
  out.session  = scalar(a["session"]);
  out.password = scalar(a["password"]);
  if (auto seq = opt_as<std::uint64_t>(a["sequence"])) out.sequence = *seq;
  return out;
}

WaitHeartbeatArgs parse_wait_args(const YAML::Node& a) {
  WaitHeartbeatArgs out{};
  out.from = scalar(a["from"], "server");
  if (auto c = opt_as<int>(a["count"]))      out.count = *c;
  if (auto t = opt_as<int>(a["timeout_ms"])) out.timeout_ms = *t;
  return out;
}

PlaceArgs parse_place_args(const YAML::Node& a) {
  PlaceArgs out{};
  out.symbol         = scalar(a["symbol"]);
  out.side           = side_from(scalar(a["side"], "BUY"));
  if (auto q = opt_as<std::int64_t>(a["qty"])) out.quantity = *q;
  out.price_str      = scalar(a["price"]);
  out.tif            = tif_from(scalar(a["tif"], "DAY"));
  out.category       = category_from(scalar(a["category"], "CLIENT"));
  out.client_account = scalar(a["afk"]);
  out.customer_info  = scalar(a["customer_info"]);
  out.exchange_info  = scalar(a["exchange_info"]);
  out.token          = scalar(a["token"]);
  if (auto d = opt_as<std::int64_t>(a["display_qty"])) out.display_quantity = *d;
  return out;
}

CancelByTokenArgs parse_cancel_token_args(const YAML::Node& a) {
  CancelByTokenArgs out{};
  out.token = scalar(a["token"]);
  return out;
}

CancelByOrderIdArgs parse_cancel_id_args(const YAML::Node& a) {
  CancelByOrderIdArgs out{};
  out.token_ref = scalar(a["token_ref"]);
  return out;
}

ReplaceArgs parse_replace_args(const YAML::Node& a) {
  ReplaceArgs out{};
  out.existing_token = scalar(a["existing_token"]);
  out.new_token      = scalar(a["new_token"]);
  if (auto q = opt_as<std::int64_t>(a["qty"])) out.quantity = *q;
  out.price_str      = scalar(a["price"]);
  out.category       = category_from(scalar(a["category"], "CLIENT"));
  out.client_account = scalar(a["afk"]);
  if (auto d = opt_as<std::int64_t>(a["display_qty"])) out.display_quantity = *d;
  return out;
}

QuoteEntrySpec parse_quote_entry(const YAML::Node& e) {
  QuoteEntrySpec q{};
  q.symbol     = scalar(e["symbol"]);
  q.bid_px     = scalar(e["bid_px"]);
  q.offer_px   = scalar(e["offer_px"]);
  if (auto v = opt_as<std::int64_t>(e["bid_size"]))   q.bid_size   = *v;
  if (auto v = opt_as<std::int64_t>(e["offer_size"])) q.offer_size = *v;
  return q;
}

MassQuoteArgs parse_mass_quote_args(const YAML::Node& a) {
  MassQuoteArgs out{};
  out.token         = scalar(a["token"]);
  out.category      = category_from(scalar(a["category"], "CLIENT"));
  out.afk           = scalar(a["afk"]);
  out.exchange_info = scalar(a["exchange_info"]);
  if (a["entries"] && a["entries"].IsSequence()) {
    for (const auto& e : a["entries"]) {
      out.entries.push_back(parse_quote_entry(e));
    }
  }
  return out;
}

FixLogonArgs parse_fix_logon_args(const YAML::Node& a) {
  FixLogonArgs out{};
  out.password      = scalar(a["password"]);
  out.new_password  = scalar(a["new_password"]);
  if (auto sr = opt_as<bool>(a["sequence_reset"])) out.sequence_reset = *sr;
  if (auto hb = opt_as<std::uint32_t>(a["heartbeat_secs"])) out.heartbeat_secs = *hb;
  return out;
}

FixAmrSubscribeArgs parse_fix_amr_args(const YAML::Node& a) {
  FixAmrSubscribeArgs out{};
  out.appl_req_id = scalar(a["appl_req_id"]);
  return out;
}

FixWaitHeartbeatArgs parse_fix_wait_args(const YAML::Node& a) {
  FixWaitHeartbeatArgs out{};
  out.from = scalar(a["from"], "server");
  if (auto c = opt_as<int>(a["count"]))      out.count = *c;
  if (auto t = opt_as<int>(a["timeout_ms"])) out.timeout_ms = *t;
  return out;
}

FixPlaceArgs parse_fix_place_args(const YAML::Node& a) {
  FixPlaceArgs out{};
  out.cl_ord_id      = scalar(a["cl_ord_id"], scalar(a["token"]));
  out.symbol         = scalar(a["symbol"]);
  out.side           = side_from(scalar(a["side"], "BUY"));
  if (auto q = opt_as<std::int64_t>(a["qty"])) out.quantity = *q;
  out.price_str      = scalar(a["price"]);
  out.tif            = tif_from(scalar(a["tif"], "DAY"));
  out.ord_type       = scalar(a["ord_type"], "LIMIT");
  out.category       = category_from(scalar(a["category"], "CLIENT"));
  out.account        = scalar(a["account"]);
  out.afk            = scalar(a["afk"]);
  if (auto d = opt_as<std::int64_t>(a["display_qty"])) out.display_quantity = *d;
  return out;
}

FixCancelArgs parse_fix_cancel_args(const YAML::Node& a) {
  FixCancelArgs out{};
  // Cert YAMLs use the OUCH-style `token` (target of cancel) plus an
  // optional `new_token` (the cancel request's own ClOrdID). Map both:
  //   token       -> orig_cl_ord_id     (the order being canceled)
  //   new_token   -> cl_ord_id          (the cancel-request id)
  out.cl_ord_id      = scalar(a["cl_ord_id"], scalar(a["new_token"]));
  out.orig_cl_ord_id = scalar(a["orig_cl_ord_id"],
                              scalar(a["token_ref"], scalar(a["token"])));
  out.symbol         = scalar(a["symbol"]);
  out.side           = side_from(scalar(a["side"], "BUY"));
  if (auto q = opt_as<std::int64_t>(a["qty"])) out.quantity = *q;
  return out;
}

FixReplaceArgs parse_fix_replace_args(const YAML::Node& a) {
  FixReplaceArgs out{};
  out.cl_ord_id      = scalar(a["cl_ord_id"], scalar(a["new_token"]));
  out.orig_cl_ord_id = scalar(a["orig_cl_ord_id"], scalar(a["existing_token"]));
  out.symbol         = scalar(a["symbol"]);
  out.side           = side_from(scalar(a["side"], "BUY"));
  if (auto q = opt_as<std::int64_t>(a["qty"])) out.new_quantity = *q;
  out.price_str      = scalar(a["price"]);
  out.ord_type       = scalar(a["ord_type"], "LIMIT");
  return out;
}

}  // namespace

Scenario load_scenario(const std::string& path) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path);
  } catch (const YAML::Exception& e) {
    throw std::runtime_error("yaml parse: " + path + ": " + e.what());
  }

  Scenario s;
  s.source_path = path;
  s.name        = scalar(root["name"]);
  s.description = scalar(root["description"]);

  if (root["preconditions"] && root["preconditions"].IsMap()) {
    for (const auto& kv : root["preconditions"]) {
      s.preconditions[kv.first.as<std::string>()] =
          kv.second.IsScalar() ? kv.second.as<std::string>() : "";
    }
  }
  if (auto t = opt_as<int>(root["default_timeout_ms"])) {
    s.default_timeout_ms = *t;
  }
  if (auto m = opt_as<bool>(root["mock_skip"])) {
    s.mock_skip = *m;
  }

  // For FIX scenarios the YAML uses generic action names like `place`,
  // `cancel_by_token`, `replace` but the runner needs the FIX-typed args.
  // Detect protocol up-front so the loop below picks the right parser.
  const auto proto_it = s.preconditions.find("protocol");
  const bool is_fix = proto_it != s.preconditions.end() &&
      (proto_it->second == "fix_oe" || proto_it->second == "fix_rd");

  const YAML::Node& steps = root["steps"];
  if (!steps || !steps.IsSequence()) {
    throw std::runtime_error("scenario " + path + ": missing 'steps' sequence");
  }
  for (const auto& sn : steps) {
    Step st{};
    if (auto id = opt_as<int>(sn["id"])) st.id = *id;
    st.action = scalar(sn["action"]);

    if (sn["args"]) {
      const auto& a = sn["args"];
      if (st.action == "ouch_login")              st.args = parse_login_args(a);
      else if (st.action == "wait_heartbeat") {
        if (is_fix) st.args = parse_fix_wait_args(a);
        else        st.args = parse_wait_args(a);
      }
      else if (st.action == "place") {
        if (is_fix) st.args = parse_fix_place_args(a);
        else        st.args = parse_place_args(a);
      }
      else if (st.action == "cancel_by_token") {
        if (is_fix) st.args = parse_fix_cancel_args(a);
        else        st.args = parse_cancel_token_args(a);
      }
      else if (st.action == "cancel_by_order_id") {
        if (is_fix) st.args = parse_fix_cancel_args(a);
        else        st.args = parse_cancel_id_args(a);
      }
      else if (st.action == "replace") {
        if (is_fix) st.args = parse_fix_replace_args(a);
        else        st.args = parse_replace_args(a);
      }
      else if (st.action == "mass_quote")         st.args = parse_mass_quote_args(a);
      else if (st.action == "switch_gateway") {
        SwitchGatewayArgs sg{};
        sg.tag = scalar(a["tag"], "secondary");
        st.args = sg;
      }
      else if (st.action == "inactivate_all") {
        st.args = InactivateAllArgs{};
      }
      else if (st.action == "burst_place") {
        BurstPlaceArgs bp{};
        if (auto c = opt_as<std::uint32_t>(a["count"]))        bp.count        = *c;
        if (auto r = opt_as<std::uint32_t>(a["rate_per_sec"])) bp.rate_per_sec = *r;
        if (a["pattern"] && a["pattern"].IsSequence()) {
          for (const auto& en : a["pattern"]) {
            BurstPlacePatternEntry e{};
            e.symbol       = scalar(en["symbol"]);
            e.side         = side_from(scalar(en["side"], "BUY"));
            if (auto q = opt_as<std::int64_t>(en["qty"])) e.quantity = *q;
            e.price_str    = scalar(en["price"]);
            e.token_prefix = scalar(en["token_prefix"]);
            e.tif          = tif_from(scalar(en["tif"], "DAY"));
            e.category     = category_from(scalar(en["category"], "CLIENT"));
            bp.pattern.push_back(e);
          }
        }
        st.args = bp;
      }
      else if (st.action == "fix_logon")          st.args = parse_fix_logon_args(a);
      else if (st.action == "fix_amr_subscribe" ||
               st.action == "fix_amr")            st.args = parse_fix_amr_args(a);
      else if (st.action == "fix_wait_heartbeat") st.args = parse_fix_wait_args(a);
      else if (st.action == "fix_place")          st.args = parse_fix_place_args(a);
      else if (st.action == "fix_cancel")         st.args = parse_fix_cancel_args(a);
      else if (st.action == "fix_replace")        st.args = parse_fix_replace_args(a);
    }

    if (sn["expect"]) st.expect = parse_expect(sn["expect"]);
    s.steps.push_back(std::move(st));
  }
  return s;
}

}  // namespace bist::runner
