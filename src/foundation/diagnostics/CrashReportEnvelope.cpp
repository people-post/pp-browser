#include "foundation/diagnostics/CrashReportEnvelope.h"

#include "foundation/runtime/ProductBranding.h"
#include "common/ValueJson.h"
#include "common/PbrCompat.h"

#include <cctype>
#include <cstdint>
#include <sstream>
#include <string_view>
#include <unordered_set>

namespace pbr {

namespace {

constexpr std::size_t kMaxFrames = 16;
constexpr std::size_t kMaxTags = 12;
constexpr std::size_t kMaxSampleBytes = 2048;

std::string Trim(std::string_view in) {
  while (!in.empty() && std::isspace(static_cast<unsigned char>(in.front()))) {
    in.remove_prefix(1);
  }
  while (!in.empty() && std::isspace(static_cast<unsigned char>(in.back()))) {
    in.remove_suffix(1);
  }
  return std::string(in);
}

bool LooksLikeFrame(const std::string& line) {
  if (line.size() < 3) {
    return false;
  }
  // Signal dumps print "%p\n" (e.g. 0x7fff...); also accept bare hex.
  if (line.rfind("0x", 0) == 0 || line.rfind("0X", 0) == 0) {
    return true;
  }
  for (char c : line) {
    if (!std::isxdigit(static_cast<unsigned char>(c))) {
      return false;
    }
  }
  return line.size() >= 8;
}

std::string ExtractLoggerTag(const std::string& line) {
  // Breadcrumb format: "[E] LoggerName: message"
  const auto bracket = line.find(']');
  if (bracket == std::string::npos || bracket + 2 >= line.size()) {
    return {};
  }
  std::size_t start = bracket + 1;
  while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) {
    ++start;
  }
  const auto colon = line.find(':', start);
  if (colon == std::string::npos || colon == start) {
    return {};
  }
  return Trim(line.substr(start, colon - start));
}

std::string Fnv1a64Hex(std::string_view data) {
  std::uint64_t hash = 14695981039346656037ull;
  for (unsigned char c : data) {
    hash ^= static_cast<std::uint64_t>(c);
    hash *= 1099511628211ull;
  }
  std::ostringstream out;
  out << std::hex << hash;
  return out.str();
}

std::string ValueAfterPrefix(const std::string& line, const char* prefix) {
  const std::size_t n = std::char_traits<char>::length(prefix);
  if (line.compare(0, n, prefix) != 0) {
    return {};
  }
  return Trim(line.substr(n));
}

} // namespace

CrashReportEnvelope BuildCrashReportEnvelope(const std::string& dump_text) {
  CrashReportEnvelope env;
  env.product = kProductSlug;
  env.sample = dump_text.size() <= kMaxSampleBytes ? dump_text : dump_text.substr(0, kMaxSampleBytes);

  bool in_breadcrumbs = false;
  std::unordered_set<std::string> seen_tags;
  std::istringstream in(dump_text);
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line == "--- breadcrumbs ---") {
      in_breadcrumbs = true;
      continue;
    }
    if (!in_breadcrumbs) {
      if (auto v = ValueAfterPrefix(line, "product="); !v.empty()) {
        env.product = std::move(v);
      } else if (auto v = ValueAfterPrefix(line, "version="); !v.empty()) {
        env.version = std::move(v);
      } else if (auto v = ValueAfterPrefix(line, "os="); !v.empty()) {
        env.os = std::move(v);
      } else if (auto v = ValueAfterPrefix(line, "reason="); !v.empty()) {
        env.reason = std::move(v);
      } else if (LooksLikeFrame(line) && env.frames.size() < kMaxFrames) {
        env.frames.push_back(line);
      }
      continue;
    }
    if (env.breadcrumb_tags.size() >= kMaxTags) {
      continue;
    }
    const std::string tag = ExtractLoggerTag(line);
    if (tag.empty() || seen_tags.count(tag) != 0) {
      continue;
    }
    seen_tags.insert(tag);
    env.breadcrumb_tags.push_back(tag);
  }

  std::ostringstream seed;
  seed << env.version << '|' << env.reason;
  for (const std::string& frame : env.frames) {
    seed << '|' << frame;
  }
  env.fingerprint = Fnv1a64Hex(seed.str());
  return env;
}

std::string CrashReportEnvelopeToJson(const CrashReportEnvelope& envelope) {
  Object root;
  root.set("schema_version", static_cast<int64_t>(envelope.schema_version));
  root.set("product", envelope.product);
  root.set("version", envelope.version);
  root.set("os", envelope.os);
  root.set("reason", envelope.reason);
  root.set("fingerprint", envelope.fingerprint);

  std::vector<Value> frames;
  frames.reserve(envelope.frames.size());
  for (const std::string& frame : envelope.frames) {
    frames.emplace_back(frame);
  }
  root.set("frames", makeArray(std::move(frames)));

  std::vector<Value> tags;
  tags.reserve(envelope.breadcrumb_tags.size());
  for (const std::string& tag : envelope.breadcrumb_tags) {
    tags.emplace_back(tag);
  }
  root.set("breadcrumb_tags", makeArray(std::move(tags)));
  root.set("sample", envelope.sample);
  return DumpJson(root);
}

std::string ResolveCrashReportUploadUrl(const std::string& relay_base_url,
                                        const std::string& crash_reports_url_override) {
  if (!crash_reports_url_override.empty()) {
    return crash_reports_url_override;
  }
  if (relay_base_url.empty()) {
    return {};
  }
  std::string base = relay_base_url;
  while (!base.empty() && base.back() == '/') {
    base.pop_back();
  }
  return base + "/v1/crash-reports";
}

} // namespace pbr
