// src/config/config.cpp — TOML loader for `bist::config::Config`.

#include "bist/config/config.hpp"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <toml++/toml.hpp>

namespace bist::config {

namespace {

template <typename T>
T get_or(const toml::node_view<const toml::node>& n, T def) {
  if (!n) return def;
  if (auto v = n.value<T>(); v) return *v;
  return def;
}

template <typename T>
void set_if_present(const toml::node_view<const toml::node>& n, T& dst) {
  if (!n) return;
  if (auto v = n.value<T>(); v) dst = *v;
}

// String overload: treat empty source string as "absent" so that the public
// TOML can ship "" as a placeholder for a value the .local.toml is expected
// to fill.
void set_if_nonempty(const toml::node_view<const toml::node>& n, std::string& dst) {
  if (!n) return;
  if (auto v = n.value<std::string>(); v && !v->empty()) dst = *v;
}

void load_ports(const toml::node_view<const toml::node>& n,
                std::vector<std::uint16_t>& out) {
  if (!n) return;
  if (const auto* arr = n.as_array(); arr) {
    out.clear();
    for (const auto& el : *arr) {
      if (auto v = el.value<std::int64_t>(); v && *v >= 0 && *v <= 0xFFFF) {
        out.push_back(static_cast<std::uint16_t>(*v));
      }
    }
  }
}

void apply(const toml::table& t, Config& cfg) {
  // [environment]
  if (auto env = t["environment"]; env.is_table()) {
    set_if_nonempty(env["name"],        cfg.environment.name);
    set_if_nonempty(env["description"], cfg.environment.description);
    set_if_present (env["weekend"],     cfg.environment.weekend);
  }

  // [ouch]
  if (auto ou = t["ouch"]; ou.is_table()) {
    set_if_nonempty(ou["host_primary"],   cfg.ouch.host_primary);
    set_if_nonempty(ou["host_secondary"], cfg.ouch.host_secondary);
    load_ports     (ou["ports"],          cfg.ouch.ports);
    set_if_present (ou["heartbeat_ms"],   cfg.ouch.heartbeat_ms);
    set_if_nonempty(ou["username"],       cfg.ouch.username);
    set_if_nonempty(ou["password"],       cfg.ouch.password);
    set_if_nonempty(ou["session"],        cfg.ouch.session);
    set_if_present (ou["sequence"],       cfg.ouch.sequence);
    set_if_present (ou["cod_reconnect_deadline_ms"],
                    cfg.ouch.cod_reconnect_deadline_ms);

    if (auto uea = ou["uea"]; uea.is_table()) {
      set_if_nonempty(uea["host"],  cfg.ouch.uea.host);
      load_ports     (uea["ports"], cfg.ouch.uea.ports);
    }
  }

  // [fix]
  if (auto fx = t["fix"]; fx.is_table()) {
    set_if_nonempty(fx["host_primary"],   cfg.fix.host_primary);
    set_if_nonempty(fx["host_colo"],      cfg.fix.host_colo);
    if (auto p = fx["oe_port"].value<std::int64_t>(); p) cfg.fix.oe_port = static_cast<std::uint16_t>(*p);
    if (auto p = fx["rd_port"].value<std::int64_t>(); p) cfg.fix.rd_port = static_cast<std::uint16_t>(*p);
    if (auto p = fx["dc_port"].value<std::int64_t>(); p) cfg.fix.dc_port = static_cast<std::uint16_t>(*p);
    set_if_nonempty(fx["sender_comp_id"],       cfg.fix.sender_comp_id);
    set_if_nonempty(fx["target_comp_id"],       cfg.fix.target_comp_id);
    set_if_nonempty(fx["username"],             cfg.fix.username);
    set_if_nonempty(fx["data_dictionary"],      cfg.fix.data_dictionary);
    set_if_nonempty(fx["transport_dictionary"], cfg.fix.transport_dictionary);
    set_if_present (fx["heartbeat_secs"],       cfg.fix.heartbeat_secs);
    set_if_nonempty(fx["session_storage"],      cfg.fix.session_storage);

    if (auto pw = fx["password"]; pw.is_table()) {
      set_if_nonempty(pw["current"], cfg.fix.password.current);
      set_if_nonempty(pw["next"],    cfg.fix.password.next);
    }

    auto load_channel = [&](const auto& node, FixChannelCfg& dst) {
      if (!node.is_table()) return;
      set_if_nonempty(node["sender_comp_id"], dst.sender_comp_id);
      set_if_nonempty(node["target_comp_id"], dst.target_comp_id);
      set_if_nonempty(node["username"],       dst.username);
    };
    load_channel(fx["oe"], cfg.fix.oe);
    load_channel(fx["rd"], cfg.fix.rd);
    load_channel(fx["dc"], cfg.fix.dc);
  }

  // [throttler]
  if (auto th = t["throttler"]; th.is_table()) {
    set_if_present(th["orders_per_sec"], cfg.throttler.orders_per_sec);
  }

  // [risk]
  if (auto rk = t["risk"]; rk.is_table()) {
    set_if_present(rk["max_qty_per_order"],    cfg.risk.max_qty_per_order);
    set_if_present(rk["max_open_orders"],      cfg.risk.max_open_orders);
    set_if_present(rk["allowed_price_min_tl"], cfg.risk.allowed_price_min_tl);
    set_if_present(rk["allowed_price_max_tl"], cfg.risk.allowed_price_max_tl);
  }

  // [runner]
  if (auto rn = t["runner"]; rn.is_table()) {
    set_if_present (rn["default_timeout_ms"], cfg.runner.default_timeout_ms);
    set_if_present (rn["abort_on_failure"],   cfg.runner.abort_on_failure);
  }

  // [logging]
  if (auto lg = t["logging"]; lg.is_table()) {
    set_if_nonempty(lg["level"],              cfg.logging.level);
    set_if_nonempty(lg["directory"],          cfg.logging.directory);
    set_if_nonempty(lg["audit_dir"],          cfg.logging.audit_dir);
    set_if_present (lg["rotate_at_size_mb"],  cfg.logging.rotate_at_size_mb);
    set_if_present (lg["keep_files"],         cfg.logging.keep_files);
  }

  // [reactor]
  if (auto re = t["reactor"]; re.is_table()) {
    if (auto v = re["hot_cpu_pin"].value<std::int64_t>(); v) cfg.reactor.hot_cpu_pin = static_cast<int>(*v);
    set_if_present(re["busy_poll"],      cfg.reactor.busy_poll);
    set_if_present(re["tcp_nodelay"],    cfg.reactor.tcp_nodelay);
    set_if_present(re["recv_buf_bytes"], cfg.reactor.recv_buf_bytes);
    set_if_present(re["send_buf_bytes"], cfg.reactor.send_buf_bytes);
  }
}

Result<void> validate(const Config& cfg) {
  if (cfg.environment.name.empty()) {
    return make_error(ErrorCategory::Validation, "config: [environment].name is empty");
  }
  if (cfg.ouch.host_primary.empty()) {
    return make_error(ErrorCategory::Validation, "config: [ouch].host_primary is empty");
  }
  if (cfg.ouch.ports.empty()) {
    return make_error(ErrorCategory::Validation, "config: [ouch].ports is empty");
  }
  if (cfg.ouch.username.empty()) {
    return make_error(ErrorCategory::Validation,
                      "config: [ouch].username is empty (set in *.local.toml)");
  }
  if (cfg.ouch.password.empty()) {
    return make_error(ErrorCategory::Validation,
                      "config: [ouch].password is empty (set in *.local.toml)");
  }
  if (cfg.throttler.orders_per_sec == 0) {
    return make_error(ErrorCategory::Validation,
                      "config: [throttler].orders_per_sec must be > 0");
  }
  return {};
}

}  // namespace

Result<Config> load_config(const std::string& path) {
  Config cfg;

  toml::table base;
  try {
    base = toml::parse_file(path);
  } catch (const toml::parse_error& e) {
    return make_error(ErrorCategory::Validation,
                      std::string{"toml parse failed for "} + path + ": " + e.what());
  } catch (const std::exception& e) {
    return make_error(ErrorCategory::Io,
                      std::string{"failed to read "} + path + ": " + e.what());
  }
  apply(base, cfg);

  // Sibling `<stem>.local.toml` overrides — secrets / per-host tweaks.
  std::filesystem::path p{path};
  auto stem = p.stem().string();      // e.g. "bist_prod_like"
  auto local_path = p.parent_path() / (stem + ".local.toml");
  if (std::filesystem::exists(local_path)) {
    try {
      auto local = toml::parse_file(local_path.string());
      apply(local, cfg);
    } catch (const toml::parse_error& e) {
      return make_error(ErrorCategory::Validation,
                        std::string{"toml parse failed for "} + local_path.string()
                        + ": " + e.what());
    }
  }

  if (auto v = validate(cfg); !v) return v.error();
  return cfg;
}

Result<ResolvedEndpoint> resolve_ouch_endpoint(const OuchCfg& cfg, int partition,
                                               bool secondary) {
  if (partition < 1 || static_cast<std::size_t>(partition) > cfg.ports.size()) {
    return make_error(ErrorCategory::Validation,
                      "partition out of range for [ouch].ports");
  }
  ResolvedEndpoint out{};
  out.host = secondary && !cfg.host_secondary.empty()
                 ? cfg.host_secondary
                 : cfg.host_primary;
  out.port = cfg.ports[static_cast<std::size_t>(partition - 1)];
  return out;
}

}  // namespace bist::config
