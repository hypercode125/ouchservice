// apps/tools/scenario_lint.cpp — schema linter for cert scenario YAMLs.
//
// Run as a CMake/CI gate before any scenario reaches the gateway. The schema
// enforced here grows alongside the runner; today it validates:
//
//   - top-level keys: name, preconditions.protocol, steps
//   - protocol value is one of {ouch, fix_oe, fix_rd, drop_copy}
//   - step ids unique and monotonically increasing
//   - action is in the protocol-specific allow-list
//   - token (or cl_ord_id) length ≤ 14 bytes
//   - tokens are not reused inside a scenario unless the offending step
//     expects an order_rejected (cert duplicate-token reject path)
//   - expect.msg is in the runner's known event set
//   - expect.fields keys are validated against the runner's known field set
//   - mock_skip flag is boolean

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <yaml-cpp/yaml.h>

namespace {

struct LintIssue {
  std::string file;
  std::string where;
  std::string message;
};

// Allow-lists keyed by protocol. Keep these in sync with
// src/runner/scenario_loader.cpp and include/bist/runner/replay.hpp.
const std::unordered_set<std::string>& valid_protocols() {
  static const std::unordered_set<std::string> s{"ouch", "fix_oe", "fix_rd",
                                                  "drop_copy"};
  return s;
}

const std::unordered_set<std::string>& ouch_actions() {
  static const std::unordered_set<std::string> s{
      "ouch_login", "ouch_logout", "wait_heartbeat", "expect",
      "place", "cancel_by_token", "cancel_by_order_id", "replace",
      "mass_quote", "switch_gateway", "burst_place", "trigger_opening_match",
      "inactivate_all"};
  return s;
}

const std::unordered_set<std::string>& fix_actions() {
  static const std::unordered_set<std::string> s{
      "fix_logon", "fix_logout", "fix_amr_subscribe", "fix_amr",
      "fix_wait_heartbeat", "wait_heartbeat",
      "fix_place", "fix_cancel", "fix_replace", "fix_trade_report",
      "expect", "trigger_match", "trigger_opening_match",
      // generic aliases that the loader maps onto FIX-typed args:
      "place", "cancel_by_token", "cancel_by_order_id", "replace"};
  return s;
}

const std::unordered_set<std::string>& ouch_event_types() {
  static const std::unordered_set<std::string> s{
      "any", "order_accepted", "order_rejected", "order_replaced",
      "order_canceled", "order_executed", "mass_quote_ack",
      "mass_quote_rejection", "login_accepted", "login_rejected",
      "socket_closed", "server_heartbeat"};
  return s;
}

const std::unordered_set<std::string>& fix_event_types() {
  static const std::unordered_set<std::string> s{
      "any", "fix_logon_response", "fix_logout_complete",
      "fix_execution_report", "fix_cancel_reject", "fix_trade_report",
      "fix_amr_ack", "fix_session_status", "server_heartbeat"};
  return s;
}

const std::unordered_set<std::string>& ouch_field_keys() {
  static const std::unordered_set<std::string> s{
      "token", "order_book_id", "side", "order_id", "quantity",
      "leaves_quantity", "traded_quantity", "price", "time_in_force",
      "open_close", "order_state", "pre_trade_quantity", "display_quantity",
      "client_category", "off_hours", "smp_level", "smp_method", "smp_id",
      "reject_code", "reason", "cancel_reason_name", "quote_status",
      "trade_price", "previous_token", "previous_order_token",
      "replacement_token", "replacement_order_token",
      "session", "next_sequence", "sequence", "reason_code"};
  return s;
}

const std::unordered_set<std::string>& fix_field_keys() {
  static const std::unordered_set<std::string> s{
      "MsgType", "ClOrdID", "OrigClOrdID", "OrderID", "ExecType",
      "OrdStatus", "OrdRejReason", "CxlRejReason", "Symbol", "Side",
      "OrderQty", "LeavesQty", "CumQty", "Price", "LastPx", "LastQty",
      "SessionStatus", "ApplReqID", "ApplResponseType", "session_status",
      "appl_response_type", "appl_req_id", "ok", "detail",
      // BIST FIX 5.0 SP2 custom fields
      "trd_type", "TrdType", "trade_type"};
  return s;
}

bool is_protocol(const std::string& p, const char* needle) {
  return p == needle;
}

// Action classification for token tracking: only producer actions create
// fresh tokens; consumer/reference actions (cancel/replace.existing_token)
// must reference an already-used token rather than be flagged as duplicates.
bool action_produces_token(const std::string& action) {
  return action == "place" || action == "fix_place" ||
         action == "mass_quote" || action == "burst_place";
}

bool action_produces_new_token_via_replace(const std::string& action) {
  return action == "replace" || action == "fix_replace";
}

void check_step_token(const std::string& action, const YAML::Node& args,
                      std::size_t idx, const std::string& file,
                      std::vector<LintIssue>& issues,
                      std::unordered_map<std::string, std::size_t>& token_seen,
                      bool expects_reject) {
  // Length check applies to every token-shaped value regardless of action.
  static const std::vector<std::string> length_check_keys{
      "token", "cl_ord_id", "new_token", "existing_token", "orig_cl_ord_id"};
  for (const auto& key : length_check_keys) {
    if (!args[key] || !args[key].IsScalar()) continue;
    const std::string tok = args[key].as<std::string>();
    if (tok.size() > 14) {
      issues.push_back({file, "steps[" + std::to_string(idx) + "].args." + key,
                        "token '" + tok + "' exceeds Alpha(14)"});
    }
  }

  // Duplicate-detection only applies to producer keys.
  std::vector<std::string> producer_keys;
  if (action_produces_token(action)) {
    producer_keys = {"token", "cl_ord_id"};
  } else if (action_produces_new_token_via_replace(action)) {
    producer_keys = {"new_token", "cl_ord_id"};
  }
  for (const auto& key : producer_keys) {
    if (!args[key] || !args[key].IsScalar()) continue;
    const std::string tok = args[key].as<std::string>();
    if (tok.empty()) continue;
    auto it = token_seen.find(tok);
    if (it != token_seen.end() && !expects_reject) {
      issues.push_back({file, "steps[" + std::to_string(idx) + "].args." + key,
                        "token '" + tok + "' already used at step index " +
                        std::to_string(it->second) +
                        " (add expect: order_rejected if duplicate is intentional)"});
    } else {
      token_seen[tok] = idx;
    }
  }
}

bool check_step(const YAML::Node& step, std::size_t idx,
                const std::string& file,
                const std::string& protocol,
                int& last_id,
                std::unordered_set<int>& seen_ids,
                std::unordered_map<std::string, std::size_t>& token_seen,
                std::vector<LintIssue>& issues) {
  bool ok = true;

  // id
  int sid = -1;
  if (!step["id"] || !step["id"].IsScalar()) {
    issues.push_back({file, "steps[" + std::to_string(idx) + "]",
                      "missing or non-scalar 'id'"});
    ok = false;
  } else {
    try {
      sid = step["id"].as<int>();
    } catch (...) {
      issues.push_back({file, "steps[" + std::to_string(idx) + "].id",
                        "must be integer"});
      ok = false;
    }
    if (sid >= 0) {
      if (!seen_ids.insert(sid).second) {
        issues.push_back({file, "steps[" + std::to_string(idx) + "].id",
                          "duplicate step id " + std::to_string(sid)});
        ok = false;
      }
      if (sid <= last_id) {
        issues.push_back({file, "steps[" + std::to_string(idx) + "].id",
                          "step id " + std::to_string(sid) +
                          " not greater than previous " + std::to_string(last_id)});
      }
      last_id = sid;
    }
  }

  // action
  std::string action;
  if (!step["action"] || !step["action"].IsScalar()) {
    issues.push_back({file, "steps[" + std::to_string(idx) + "]",
                      "missing or non-scalar 'action'"});
    ok = false;
  } else {
    action = step["action"].as<std::string>();
    const auto& allow = is_protocol(protocol, "ouch") ? ouch_actions()
                                                       : fix_actions();
    if (allow.find(action) == allow.end()) {
      issues.push_back({file, "steps[" + std::to_string(idx) + "].action",
                        "action '" + action + "' not in " + protocol +
                        " allow-list"});
      ok = false;
    }
  }

  // expect
  bool expects_reject = false;
  if (step["expect"]) {
    const YAML::Node& exp = step["expect"];
    std::string msg;
    if (exp.IsScalar()) {
      msg = exp.as<std::string>();
    } else if (exp.IsMap()) {
      if (!exp["msg"]) {
        issues.push_back({file, "steps[" + std::to_string(idx) + "].expect",
                          "map form requires a 'msg' key"});
        ok = false;
      } else if (exp["msg"].IsScalar()) {
        msg = exp["msg"].as<std::string>();
      }
      if (exp["fields"] && exp["fields"].IsMap()) {
        const auto& field_keys = is_protocol(protocol, "ouch")
                                      ? ouch_field_keys()
                                      : fix_field_keys();
        for (const auto& kv : exp["fields"]) {
          const std::string k = kv.first.as<std::string>();
          if (field_keys.find(k) == field_keys.end()) {
            issues.push_back({file, "steps[" + std::to_string(idx) + "].expect.fields",
                              "field key '" + k + "' not in " + protocol +
                              " known field set"});
            ok = false;
          }
        }
      }
    } else {
      issues.push_back({file, "steps[" + std::to_string(idx) + "].expect",
                        "must be scalar shorthand or map"});
      ok = false;
    }
    if (!msg.empty()) {
      const auto& events = is_protocol(protocol, "ouch") ? ouch_event_types()
                                                          : fix_event_types();
      if (events.find(msg) == events.end()) {
        issues.push_back({file, "steps[" + std::to_string(idx) + "].expect.msg",
                          "event type '" + msg + "' not in " + protocol +
                          " known event set"});
        ok = false;
      }
      if (msg == "order_rejected" || msg == "fix_cancel_reject") {
        expects_reject = true;
      }
    }
  }

  if (step["args"] && step["args"].IsMap()) {
    check_step_token(action, step["args"], idx, file, issues, token_seen,
                     expects_reject);
  }

  return ok;
}

bool lint_one(const std::filesystem::path& path,
              std::vector<LintIssue>& issues) {
  YAML::Node root;
  try {
    root = YAML::LoadFile(path.string());
  } catch (const YAML::Exception& e) {
    issues.push_back({path.string(), "(parse)", e.what()});
    return false;
  }

  const std::string file = path.string();
  if (!root["name"] || !root["name"].IsScalar()) {
    issues.push_back({file, "(root)", "missing or non-scalar 'name'"});
  }

  std::string protocol = "ouch";
  if (root["preconditions"] && root["preconditions"].IsMap()) {
    if (root["preconditions"]["protocol"] &&
        root["preconditions"]["protocol"].IsScalar()) {
      protocol = root["preconditions"]["protocol"].as<std::string>();
      if (valid_protocols().find(protocol) == valid_protocols().end()) {
        issues.push_back({file, "preconditions.protocol",
                          "protocol '" + protocol + "' not in {ouch, fix_oe, "
                          "fix_rd, drop_copy}"});
      }
    } else {
      issues.push_back({file, "preconditions",
                        "missing required 'protocol' key"});
    }
  } else {
    issues.push_back({file, "(root)",
                      "missing 'preconditions' map (must declare protocol)"});
  }

  if (root["mock_skip"] && !root["mock_skip"].IsScalar()) {
    issues.push_back({file, "mock_skip", "must be boolean"});
  }

  if (!root["steps"] || !root["steps"].IsSequence()) {
    issues.push_back({file, "(root)", "missing or non-sequence 'steps'"});
    return false;
  }

  int last_id = -1;
  std::unordered_set<int> seen_ids;
  std::unordered_map<std::string, std::size_t> token_seen;
  std::size_t idx = 0;
  for (const auto& step : root["steps"]) {
    if (!step.IsMap()) {
      issues.push_back({file, "steps[" + std::to_string(idx) + "]",
                        "must be a map"});
    } else {
      check_step(step, idx, file, protocol, last_id, seen_ids, token_seen,
                 issues);
    }
    ++idx;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: %s <scenario.yaml | dir> [...]\n", argv[0]);
    return EXIT_FAILURE;
  }

  std::vector<std::filesystem::path> targets;
  for (int i = 1; i < argc; ++i) {
    std::filesystem::path p{argv[i]};
    if (std::filesystem::is_directory(p)) {
      for (const auto& entry : std::filesystem::recursive_directory_iterator(p)) {
        if (entry.is_regular_file() &&
            (entry.path().extension() == ".yaml" ||
             entry.path().extension() == ".yml")) {
          targets.push_back(entry.path());
        }
      }
    } else {
      targets.push_back(p);
    }
  }
  std::sort(targets.begin(), targets.end());

  std::vector<LintIssue> issues;
  for (const auto& t : targets) lint_one(t, issues);

  if (issues.empty()) {
    std::printf("ok: %zu file(s) clean\n", targets.size());
    return EXIT_SUCCESS;
  }
  for (const auto& iss : issues) {
    std::fprintf(stderr, "%s: %s: %s\n",
                 iss.file.c_str(), iss.where.c_str(), iss.message.c_str());
  }
  std::fprintf(stderr, "%zu issue(s) across %zu file(s)\n",
               issues.size(), targets.size());
  return EXIT_FAILURE;
}
