#include "domain/attachments/SentRelayFile.h"

#include "domain/attachments/RelayFileUrl.h"
#include "lib/delivery/ChatPayloadCodec.h"

#include "common/CivilTime.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <optional>

namespace pbr {

namespace {

/** The relay's ISO-8601 UTC time (`2026-11-29T07:02:00Z`, fractions ignored) in Unix ms; none when it is not one. */
std::optional<int64_t> RelayTimeMs(const std::string& iso) {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  if (iso.size() < 19 ||
      std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%d", &year, &month, &day, &hour, &minute, &second) != 6) {
    return std::nullopt;
  }
  std::tm tm{};
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  const time_t seconds = civil_time::TimeGm(&tm);
  if (seconds == static_cast<time_t>(-1)) {
    return std::nullopt;
  }
  return static_cast<int64_t>(seconds) * 1000;
}

} // namespace

bool SentRelayFileGone(const std::string& payload_json, const int64_t sent_at_ms, const int64_t now_ms,
                       const RelayFileListResult* our_files) {
  auto fields = ChatPayloadCodec::DecodeAttachmentJson(payload_json);
  if (!fields || fields->url.empty()) {
    return false;
  }
  const std::string blob_id = RelayFileUrl::BlobIdOf(fields->url);
  if (blob_id.empty()) {
    return false;
  }
  if (our_files != nullptr) {
    const auto listed = std::find_if(our_files->files.begin(), our_files->files.end(),
                                     [&blob_id](const RelayFileRecord& file) { return file.blob_id == blob_id; });
    if (listed != our_files->files.end()) {
      // The relay says until when it keeps it: that, not our guess from the send time.
      const std::optional<int64_t> expires_ms = RelayTimeMs(listed->expires_at);
      return expires_ms && now_ms >= *expires_ms;
    }
    if (our_files->files.size() < kRelayFileListCap) {
      return true; // a complete list without it: deleted (Me > Storage) or expired
    }
  }
  return now_ms - sent_at_ms > kRelayFileLifetimeMs; // not known from the relay: its stated lifetime
}

} // namespace pbr
