#include "foundation/diagnostics/CrashBreadcrumbs.h"

#include "common/Logger.h"
#include "common/PbrCompat.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace pbr {

namespace {

char g_lines[CrashBreadcrumbs::kCapacity][CrashBreadcrumbs::kLineBytes]{};
std::atomic<std::uint32_t> g_seq{0};
std::mutex g_install_mu;
bool g_handler_installed = false;

const char* LevelTag(logging::Level level) {
  switch (level) {
  case logging::kLevelDebug:
    return "D";
  case logging::Level::INFO:
    return "I";
  case logging::Level::WARNING:
    return "W";
  case logging::kLevelError:
  case logging::Level::CRITICAL:
    return "E";
  }
  return "I";
}

class BreadcrumbHandler final : public logging::Handler {
public:
  void emit(logging::Level level, const std::string& loggerName, const std::string& message) override {
    if (level < level_) {
      return;
    }
    char buf[CrashBreadcrumbs::kLineBytes];
    std::snprintf(buf, sizeof(buf), "[%s] %s: %s", LevelTag(level), loggerName.c_str(), message.c_str());
    CrashBreadcrumbs::Append(buf);
  }
};

void WriteLine(std::uint32_t slot, const char* text) {
  char* dest = g_lines[slot % CrashBreadcrumbs::kCapacity];
  if (text == nullptr) {
    dest[0] = '\0';
    return;
  }
  std::snprintf(dest, CrashBreadcrumbs::kLineBytes, "%s", text);
}

} // namespace

void CrashBreadcrumbs::InstallLoggerHandler() {
  std::lock_guard<std::mutex> lock(g_install_mu);
  if (g_handler_installed) {
    return;
  }
  logging::getRootLogger().addHandler(std::make_shared<BreadcrumbHandler>());
  g_handler_installed = true;
}

void CrashBreadcrumbs::Append(const std::string& line) {
  const std::uint32_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
  WriteLine(seq, line.c_str());
}

std::vector<std::string> CrashBreadcrumbs::Snapshot() {
  const std::uint32_t seq = g_seq.load(std::memory_order_relaxed);
  const std::size_t count = seq < kCapacity ? seq : kCapacity;
  std::vector<std::string> out;
  out.reserve(count);
  const std::uint32_t start = seq < kCapacity ? 0 : (seq - static_cast<std::uint32_t>(kCapacity));
  for (std::size_t i = 0; i < count; ++i) {
    const char* line = g_lines[(start + static_cast<std::uint32_t>(i)) % kCapacity];
    if (line[0] != '\0') {
      out.emplace_back(line);
    }
  }
  return out;
}

std::size_t CrashBreadcrumbs::CopySignalSafe(char* out, std::size_t out_bytes) {
  if (out == nullptr || out_bytes == 0) {
    return 0;
  }
  out[0] = '\0';
  if (out_bytes == 1) {
    return 0;
  }

  const std::uint32_t seq = g_seq.load(std::memory_order_relaxed);
  const std::size_t count = seq < kCapacity ? seq : kCapacity;
  const std::uint32_t start = seq < kCapacity ? 0 : (seq - static_cast<std::uint32_t>(kCapacity));

  std::size_t used = 0;
  for (std::size_t i = 0; i < count; ++i) {
    const char* line = g_lines[(start + static_cast<std::uint32_t>(i)) % kCapacity];
    if (line[0] == '\0') {
      continue;
    }
    const std::size_t len = std::strlen(line);
    if (used + len + 2 >= out_bytes) {
      break;
    }
    std::memcpy(out + used, line, len);
    used += len;
    out[used++] = '\n';
    out[used] = '\0';
  }
  return used;
}

} // namespace pbr
