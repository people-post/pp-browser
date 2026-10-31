#include "foundation/diagnostics/DiagnosticsUploadResult.h"

#include "common/PbrCompat.h"
#include "common/Utilities.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace pbr {

namespace fs = std::filesystem;

namespace {

std::string StringField(const Object& object, const char* key) {
  return object.getIf<std::string>(key).value_or(std::string());
}

} // namespace

DiagnosticsUploadResult MapDiagnosticsResponse(const long status, const std::string& body) {
  DiagnosticsUploadResult result;
  const std::optional<Object> json = TryParseObject(body);
  if (status >= 200 && status < 300) {
    if (json) {
      result.upload_id = StringField(*json, "upload_id");
      if (auto lines = json->getIf<int64_t>("lines")) {
        result.lines = static_cast<int>(*lines);
      }
    }
    if (result.upload_id.empty()) {
      result.detail = "HTTP " + std::to_string(status) + " without a reference";
      return result;
    }
    result.kind = DiagnosticsUploadKind::Sent;
    return result;
  }
  switch (status) {
  case 429: {
    const std::string reason = json ? StringField(*json, "reason") : std::string();
    if (reason == "quota_key") {
      result.kind = DiagnosticsUploadKind::QuotaKey;
    } else if (reason == "quota_ip") {
      result.kind = DiagnosticsUploadKind::QuotaIp;
    } else if (reason == "quota_global") {
      result.kind = DiagnosticsUploadKind::QuotaGlobal;
    } else {
      result.detail = "HTTP 429";
    }
    return result;
  }
  case 409:
    result.kind = DiagnosticsUploadKind::InProgress;
    return result;
  case 401:
  case 404:
  case 503:
    result.kind = DiagnosticsUploadKind::Unavailable;
    return result;
  case 413:
    result.kind = DiagnosticsUploadKind::TooLarge;
    return result;
  default:
    result.detail = "HTTP " + std::to_string(status);
    return result;
  }
}

bool DiagnosticsKindKeepsToken(const DiagnosticsUploadKind kind) {
  return kind == DiagnosticsUploadKind::NoNetwork || kind == DiagnosticsUploadKind::Failed ||
         kind == DiagnosticsUploadKind::InProgress;
}

const std::string& DiagnosticsClientToken::Acquire() {
  if (token_.empty()) {
    token_ = util::GenerateUuid();
  }
  return token_;
}

void DiagnosticsClientToken::Finish(const DiagnosticsUploadKind kind) {
  if (!DiagnosticsKindKeepsToken(kind)) {
    token_.clear();
  }
}

std::string LastUploadPath(const std::string& data_dir) {
  return (fs::path(data_dir) / "diagnostics" / "last_upload.json").string();
}

bool WriteLastUpload(const std::string& data_dir, const LastUpload& last) {
  const std::string path = LastUploadPath(data_dir);
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  {
    std::ofstream touch(path, std::ios::app);  // exists and is locked down before the id is written
  }
  fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write, fs::perm_options::replace, ec);
  Object object;
  object.set("upload_id", last.upload_id);
  object.set("at_ms", last.at_ms);
  std::ofstream out(path, std::ios::trunc);
  out << DumpJson(object);
  return static_cast<bool>(out);
}

std::optional<LastUpload> ReadLastUpload(const std::string& data_dir) {
  std::ifstream in(LastUploadPath(data_dir));
  if (!in) {
    return std::nullopt;
  }
  const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  const std::optional<Object> json = TryParseObject(text);
  if (!json) {
    return std::nullopt;
  }
  LastUpload last;
  last.upload_id = StringField(*json, "upload_id");
  last.at_ms = json->getIf<int64_t>("at_ms").value_or(0);
  if (last.upload_id.empty()) {
    return std::nullopt;
  }
  return last;
}

} // namespace pbr
