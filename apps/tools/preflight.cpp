// apps/tools/preflight.cpp — go/no-go check before connecting to BIST.
//
// Verifies that everything the resident bist_colo needs is in place:
//   - config TOML loads + non-empty member credentials
//   - audit + state directories writable
//   - FIX dictionaries present (path stamped at build time)
//   - TCP connect to OUCH primary + secondary endpoints succeeds
//   - TCP connect to FIX OE / FIX RD ports succeeds (when configured)
//   - reports CoD reconnect deadline + throttler rate
//
// Exit code 0 means ready; any non-zero exit lists blockers and aborts
// systemd ExecStartPre so the service never reaches the gateway with a
// half-baked configuration.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "bist/config/config.hpp"
#include "bist/net/tcp_socket.hpp"

namespace fs = std::filesystem;

namespace {

struct Issue {
  bool        fatal{true};
  std::string detail;
};

void emit(std::vector<Issue>& out, bool fatal, std::string detail) {
  out.push_back({fatal, std::move(detail)});
}

void check_credential(std::vector<Issue>& out, const std::string& name,
                      const std::string& value, bool fatal = true) {
  if (value.empty()) emit(out, fatal, name + " is empty");
}

void check_writable(std::vector<Issue>& out, const fs::path& dir,
                    const char* purpose) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    emit(out, true,
         std::string{purpose} + " dir " + dir.string() +
         " not creatable: " + ec.message());
    return;
  }
  const fs::path probe = dir / ".preflight.tmp";
  std::FILE* f = std::fopen(probe.c_str(), "w");
  if (!f) {
    emit(out, true,
         std::string{purpose} + " dir " + dir.string() + " not writable");
    return;
  }
  std::fclose(f);
  std::error_code ec2;
  fs::remove(probe, ec2);
}

void check_file(std::vector<Issue>& out, const fs::path& p, const char* purpose) {
  if (p.empty()) {
    emit(out, true, std::string{purpose} + ": path is empty");
    return;
  }
  if (!fs::exists(p)) {
    emit(out, true, std::string{purpose} + " missing: " + p.string());
    return;
  }
}

void check_tcp(std::vector<Issue>& out, const std::string& host,
               std::uint16_t port, const std::string& label, bool fatal = true) {
  if (host.empty() || port == 0) {
    emit(out, fatal, label + ": host/port not configured");
    return;
  }
  bist::net::TcpSocket s;
  auto r = s.connect(host, port, /*tcp_nodelay=*/false);
  if (!r) {
    emit(out, fatal, label + ": " + host + ":" + std::to_string(port) +
                     " connect failed: " + r.error().detail);
    return;
  }
  std::printf("  ok   %-24s %s:%u\n", label.c_str(), host.c_str(), port);
}

}  // namespace

int main(int argc, char** argv) {
  std::string cfg_path;
  bool include_secondary = true;
  bool include_fix       = true;
  for (int i = 1; i < argc; ++i) {
    std::string_view a{argv[i]};
    if (a == "--config" && i + 1 < argc) cfg_path = argv[++i];
    else if (a == "--no-secondary") include_secondary = false;
    else if (a == "--no-fix")       include_fix = false;
    else if (a == "-h" || a == "--help") {
      std::printf("usage: %s --config <path> [--no-secondary] [--no-fix]\n",
                  argv[0]);
      return 0;
    } else {
      std::fprintf(stderr, "unknown arg: %s\n", argv[i]);
      return 1;
    }
  }
  if (cfg_path.empty()) {
    std::fprintf(stderr, "preflight: --config is required\n");
    return 1;
  }

  std::printf("=== bist_colo preflight ===\n");
  std::printf("config: %s\n", cfg_path.c_str());

  auto cfg_r = bist::config::load_config(cfg_path);
  if (!cfg_r) {
    std::fprintf(stderr, "  fail config: %s\n", cfg_r.error().detail.c_str());
    return 1;
  }
  const auto& cfg = cfg_r.value();
  std::printf("environment: %s (%s)\n",
              cfg.environment.name.c_str(),
              cfg.environment.description.c_str());
  std::printf("CoD reconnect deadline: %u ms (BIST inactivation 55-62s)\n",
              cfg.ouch.cod_reconnect_deadline_ms);
  std::printf("throttler: %u orders/sec\n", cfg.throttler.orders_per_sec);

  std::vector<Issue> issues;

  // --- credentials --------------------------------------------------------
  check_credential(issues, "ouch.username", cfg.ouch.username);
  check_credential(issues, "ouch.password", cfg.ouch.password);
  check_credential(issues, "ouch.host_primary", cfg.ouch.host_primary);
  if (cfg.ouch.ports.empty()) emit(issues, true, "ouch.ports list is empty");

  if (include_fix) {
    if (cfg.fix.host_primary.empty())
      emit(issues, false, "fix.host_primary empty (FIX cert disabled)");
    if (cfg.fix.oe_port == 0 && cfg.fix.rd_port == 0)
      emit(issues, false, "fix.{oe,rd}_port both zero (FIX cert disabled)");
    check_credential(issues, "fix.password.current",
                     cfg.fix.password.current,
                     /*fatal=*/!cfg.fix.host_primary.empty());
  }

  // --- filesystem ---------------------------------------------------------
  check_writable(issues, cfg.logging.audit_dir.empty() ? fs::path{"audit"}
                                                       : fs::path{cfg.logging.audit_dir},
                 "audit");
  check_writable(issues, fs::path{"state"}, "state");
  check_writable(issues, cfg.logging.directory.empty() ? fs::path{"log"}
                                                       : fs::path{cfg.logging.directory},
                 "log");

  if (include_fix && !cfg.fix.host_primary.empty()) {
    if (!cfg.fix.data_dictionary.empty())
      check_file(issues, cfg.fix.data_dictionary, "fix.data_dictionary");
    if (!cfg.fix.transport_dictionary.empty())
      check_file(issues, cfg.fix.transport_dictionary, "fix.transport_dictionary");
  }

  // --- network probes -----------------------------------------------------
  std::printf("\nnetwork probes:\n");
  if (!cfg.ouch.host_primary.empty() && !cfg.ouch.ports.empty()) {
    check_tcp(issues, cfg.ouch.host_primary, cfg.ouch.ports.front(),
              "ouch.primary[1]");
  }
  if (include_secondary && !cfg.ouch.host_secondary.empty() &&
      !cfg.ouch.ports.empty()) {
    check_tcp(issues, cfg.ouch.host_secondary, cfg.ouch.ports.front(),
              "ouch.secondary[1]", /*fatal=*/false);
  }
  if (include_fix && !cfg.fix.host_primary.empty()) {
    if (cfg.fix.oe_port != 0)
      check_tcp(issues, cfg.fix.host_primary, cfg.fix.oe_port, "fix.oe");
    if (cfg.fix.rd_port != 0)
      check_tcp(issues, cfg.fix.host_primary, cfg.fix.rd_port, "fix.rd");
    if (cfg.fix.dc_port != 0)
      check_tcp(issues, cfg.fix.host_primary, cfg.fix.dc_port, "fix.dc",
                /*fatal=*/false);
  }

  // --- summary ------------------------------------------------------------
  std::printf("\n");
  if (issues.empty()) {
    std::printf("PREFLIGHT PASS — bist_colo is ready to start.\n");
    return 0;
  }
  bool any_fatal = false;
  for (const auto& i : issues) {
    std::printf("%s %s\n", i.fatal ? "  FAIL" : "  WARN", i.detail.c_str());
    if (i.fatal) any_fatal = true;
  }
  if (any_fatal) {
    std::fprintf(stderr,
                 "\nPREFLIGHT FAIL — refusing to start. Fix the FAIL items above.\n");
    return 2;
  }
  std::printf("\nPREFLIGHT PASS (with warnings).\n");
  return 0;
}
