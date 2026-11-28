#include "domain/relay/RelaySettings.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <optional>

namespace pbr::relay {
namespace {

constexpr const char* kKeys[] = {"enabled", "retention_days", "max_records", "max_bytes"};

std::string Trim(std::string s) {
  const auto not_space = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), not_space));
  s.erase(std::find_if(s.rbegin(), s.rend(), not_space).base(), s.end());
  return s;
}

std::optional<bool> ParseBool(std::string text) {
  text = Trim(std::move(text));
  std::transform(text.begin(), text.end(), text.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (text == "1" || text == "true" || text == "yes" || text == "on") {
    return true;
  }
  if (text == "0" || text == "false" || text == "no" || text == "off") {
    return false;
  }
  return std::nullopt;
}

std::optional<int64_t> ParseInt(std::string text) {
  text = Trim(std::move(text));
  int64_t value = 0;
  const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (ec != std::errc() || end != text.data() + text.size() || text.empty()) {
    return std::nullopt;
  }
  return value;
}

std::string EnvName(const std::string& key) {
  std::string name = "PP_NODE_RELAY_";
  for (const char c : key) {
    name += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  return name;
}

Roe<void> ReadBool(const Object* section, const RelaySettings::Env& env, const std::string& key, bool& out) {
  if (section && section->entries().contains(key)) {
    const auto value = section->getIf<bool>(key);
    if (!value) {
      return Error("node.relay." + key + " must be true or false");
    }
    out = *value;
  }
  const std::string name = EnvName(key);
  if (const char* text = env ? env(name.c_str()) : nullptr; text && *text) {
    const auto value = ParseBool(text);
    if (!value) {
      return Error(name + " must be 1/0, true/false, yes/no or on/off");
    }
    out = *value;
  }
  return {};
}

Roe<void> ReadInt(const Object* section, const RelaySettings::Env& env, const std::string& key, const int64_t min,
                  const int64_t max, int64_t& out) {
  const std::string bounds = " (" + std::to_string(min) + " to " + std::to_string(max) + ")";
  if (section && section->entries().contains(key)) {
    const auto value = section->getIf<int64_t>(key);
    if (!value) {
      return Error("node.relay." + key + " must be a whole number" + bounds);
    }
    out = *value;
  }
  const std::string name = EnvName(key);
  if (const char* text = env ? env(name.c_str()) : nullptr; text && *text) {
    const auto value = ParseInt(text);
    if (!value) {
      return Error(name + " must be a whole number" + bounds);
    }
    out = *value;
  }
  if (out < min || out > max) {
    return Error("node.relay." + key + " is " + std::to_string(out) + ", outside" + bounds);
  }
  return {};
}

std::string Size(const int64_t bytes) {
  if (bytes % (1024 * 1024) == 0) {
    return std::to_string(bytes / (1024 * 1024)) + " MiB";
  }
  if (bytes % 1024 == 0) {
    return std::to_string(bytes / 1024) + " KiB";
  }
  return std::to_string(bytes) + " bytes";
}

} // namespace

Roe<RelaySettings> RelaySettings::Read(const Object* section, const Env& env, std::vector<std::string>* unknown) {
  RelaySettings s;
  if (auto r = ReadBool(section, env, "enabled", s.enabled); !r) {
    return r.error();
  }
  if (auto r = ReadInt(section, env, "retention_days", 1, kMaxRetentionDays, s.retention_days); !r) {
    return r.error();
  }
  if (auto r = ReadInt(section, env, "max_records", 1, kMaxRecordsCap, s.max_records); !r) {
    return r.error();
  }
  if (auto r = ReadInt(section, env, "max_bytes", kMinBytes, kMaxBytesCap, s.max_bytes); !r) {
    return r.error();
  }
  if (section && unknown) {
    for (const auto& [key, value] : section->entries()) {
      (void)value;
      if (std::find(std::begin(kKeys), std::end(kKeys), key) == std::end(kKeys)) {
        unknown->push_back(key);
      }
    }
  }
  return s;
}

std::string RelaySettings::Describe() const {
  if (!enabled) {
    return "off (node.relay.enabled)";
  }
  return "on, keeps " + std::to_string(retention_days) + " days, " + std::to_string(max_records) + " records / " +
         Size(max_bytes) + " per account";
}

} // namespace pbr::relay
