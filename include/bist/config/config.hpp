#pragma once
//
// bist/config/config.hpp — typed configuration loaded from TOML.
//
// The runtime owner is the main binary. A single `Config` object is built
// once at startup and passed by const-reference into the OUCH/FIX session
// layers, the throttler, the audit log, and the reactor.
//
// The on-disk schema lives in `config/bist_*.toml`. Secrets (account
// usernames, passwords, FIX 554/925 password rotation values) live in a
// sibling `*.local.toml` that the loader merges on top — the public TOML
// ships only the endpoint geometry. `*.local.toml` is gitignored.

#include <cstdint>
#include <string>
#include <vector>

#include "bist/core/result.hpp"

namespace bist::config {

struct EnvironmentCfg {
  std::string name;
  std::string description;
  bool        weekend{false};
};

struct UeaCfg {
  std::string                 host;
  std::vector<std::uint16_t>  ports;
};

struct OuchCfg {
  std::string                 host_primary;
  std::string                 host_secondary;
  std::vector<std::uint16_t>  ports;            // index 0 = partition 1
  std::uint32_t               heartbeat_ms{800};
  std::string                 username;
  std::string                 password;
  std::string                 session;          // "" = "any session"
  std::uint64_t               sequence{0};      // 0 = "from current"
  // CoD (Cancel-on-Disconnect) reconnect deadline. Default 30 s — comfortably
  // before BIST ME's 55–62 s inactivation window. Operator may shorten in
  // production-like profiles.
  std::uint32_t               cod_reconnect_deadline_ms{30'000};
  UeaCfg                      uea;
};

struct FixPasswordCfg {
  std::string current;                          // tag 554
  std::string next;                             // tag 925 (rotation on SessionStatus=8)
};

struct FixChannelCfg {
  // Per-channel SenderCompID/TargetCompID. If empty, the loader falls back
  // to FixCfg::sender_comp_id / target_comp_id with the channel suffix
  // appended (e.g. "MEMBER_OE", "MEMBER_RD").
  std::string sender_comp_id;
  std::string target_comp_id;
  std::string username;        // tag 553 (defaults to FixCfg::username)
};

struct FixCfg {
  std::string                 host_primary;
  std::string                 host_colo;
  std::uint16_t               oe_port{0};
  std::uint16_t               rd_port{0};
  std::uint16_t               dc_port{0};
  std::string                 sender_comp_id;        // base if per-channel empty
  std::string                 target_comp_id;        // base if per-channel empty
  std::string                 username;              // tag 553 default
  std::string                 data_dictionary;
  std::string                 transport_dictionary;
  std::uint32_t               heartbeat_secs{30};
  std::string                 session_storage;
  FixPasswordCfg              password;
  FixChannelCfg               oe;
  FixChannelCfg               rd;
  FixChannelCfg               dc;
};

struct ThrottlerCfg {
  std::uint32_t orders_per_sec{100};            // BIST cert default
};

struct RiskCfg {
  std::uint64_t max_qty_per_order{0};
  std::uint64_t max_open_orders{0};
  double        allowed_price_min_tl{0.0};
  double        allowed_price_max_tl{0.0};
};

struct RunnerCfg {
  std::uint32_t default_timeout_ms{5000};
  bool          abort_on_failure{true};
};

struct LoggingCfg {
  std::string   level{"info"};
  std::string   directory;
  std::string   audit_dir;
  std::uint32_t rotate_at_size_mb{256};
  std::uint32_t keep_files{30};
};

struct ReactorCfg {
  int           hot_cpu_pin{-1};
  bool          busy_poll{false};
  bool          tcp_nodelay{true};
  std::uint32_t recv_buf_bytes{1u << 20};
  std::uint32_t send_buf_bytes{1u << 20};
};

struct Config {
  EnvironmentCfg environment;
  OuchCfg        ouch;
  FixCfg         fix;
  ThrottlerCfg   throttler;
  RiskCfg        risk;
  RunnerCfg      runner;
  LoggingCfg     logging;
  ReactorCfg     reactor;
};

// Load `path` as TOML, validate, and merge a sibling `<stem>.local.toml` if
// one exists (override semantics: only non-empty keys in the local file
// replace base values).
Result<Config> load_config(const std::string& path);

// Partition is 1-based per BIST convention. Returns the OUCH host:port to
// connect to for the given partition, or a Validation error if the partition
// is out of range. `secondary=true` returns the host_secondary endpoint.
struct ResolvedEndpoint {
  std::string   host;
  std::uint16_t port{0};
};
Result<ResolvedEndpoint> resolve_ouch_endpoint(const OuchCfg& cfg,
                                               int partition,
                                               bool secondary = false);

}  // namespace bist::config
