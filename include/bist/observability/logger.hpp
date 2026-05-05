#pragma once
//
// bist/observability/logger.hpp — thin wrapper around spdlog's async logger.
//
// We keep the surface tiny so that flipping to a different logger later is a
// one-file change. All call sites use these helpers (or the SPDLOG_*
// macros directly) — no raw printf or std::cout in production code.

#include <memory>
#include <string>

#include <spdlog/async.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

namespace bist::observability {

struct LoggerConfig {
  std::string name      = "bist";
  std::string directory = "log";
  std::string filename  = "bist.log";
  std::size_t rotate_bytes = 256ULL * 1024 * 1024;
  std::size_t keep_files   = 30;
  spdlog::level::level_enum level = spdlog::level::info;
};

inline std::shared_ptr<spdlog::logger> install(const LoggerConfig& cfg) {
  spdlog::init_thread_pool(8192, 1);

  auto stdout_sink =
      std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
  auto file_sink =
      std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
          cfg.directory + "/" + cfg.filename,
          cfg.rotate_bytes,
          cfg.keep_files);

  std::vector<spdlog::sink_ptr> sinks{stdout_sink, file_sink};
  auto logger = std::make_shared<spdlog::async_logger>(
      cfg.name, sinks.begin(), sinks.end(),
      spdlog::thread_pool(), spdlog::async_overflow_policy::overrun_oldest);
  logger->set_level(cfg.level);
  logger->set_pattern("%Y-%m-%dT%H:%M:%S.%f%z [%^%l%$] [%n] %v");
  spdlog::set_default_logger(logger);
  return logger;
}

}  // namespace bist::observability
