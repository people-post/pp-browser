#include "base/net/RelayInboxCursor.h"

#include <fstream>
#include <filesystem>

#include <nlohmann/json.hpp>

namespace pbr {
namespace {

std::filesystem::path CursorPath(const std::string& profile_data_dir) {
  return std::filesystem::path(profile_data_dir) / "relay_inbox_cursor.json";
}

} // namespace

std::string LoadRelayInboxCursor(const std::string& profile_data_dir, const std::string& relay_user_id) {
  if (profile_data_dir.empty() || relay_user_id.empty()) {
    return {};
  }
  std::ifstream in(CursorPath(profile_data_dir));
  if (!in) {
    return {};
  }
  nlohmann::json root = nlohmann::json::parse(in, nullptr, false);
  if (root.is_discarded() || !root.is_object()) {
    return {};
  }
  if (!root.contains("relay_user_id") || !root["relay_user_id"].is_string() ||
      root["relay_user_id"].get<std::string>() != relay_user_id) {
    return {};
  }
  if (!root.contains("cursor") || !root["cursor"].is_string()) {
    return {};
  }
  return root["cursor"].get<std::string>();
}

void SaveRelayInboxCursor(const std::string& profile_data_dir, const std::string& relay_user_id,
                          const std::string& cursor) {
  if (profile_data_dir.empty() || relay_user_id.empty() || cursor.empty()) {
    return;
  }
  std::error_code ec;
  std::filesystem::create_directories(profile_data_dir, ec);
  const nlohmann::json root = {{"relay_user_id", relay_user_id}, {"cursor", cursor}};
  const auto path = CursorPath(profile_data_dir);
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream out(tmp, std::ios::trunc);
    if (!out) {
      return;
    }
    out << root.dump();
  }
  std::filesystem::rename(tmp, path, ec);
}

} // namespace pbr
