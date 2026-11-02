#include "lib/net/BlobClient.h"

#include "common/Utilities.h"
#include "common/PbrCompat.h"

#include <cctype>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>

namespace pbr {

namespace {

logging::Logger& RelayFilesLog() {
  static logging::Logger logger = logging::getLogger("RelayFiles");
  return logger;
}

/**
 * A single PUT URL lives 15 minutes, but a PUT started in time may still be running and device clocks drift
 * (±5 min): only a pending upload started over an hour ago is surely abandoned.
 */
constexpr int64_t kStalePendingMs = 60LL * 60LL * 1000LL;

/** The relay's ISO-8601 form (`Date.toISOString()`), so the two compare as strings. */
std::string IsoUtcMs(const int64_t unix_ms) {
  std::tm tm{};
  if (unix_ms < 0 || !civil_time::UtcTime(static_cast<time_t>(unix_ms / 1000), &tm)) {
    return {};
  }
  char text[64]; // wide enough for GCC's worst-case %d analysis (-Wformat-truncation)
  std::snprintf(text, sizeof(text), "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(unix_ms % 1000));
  return text;
}

/** Delete this account's pending uploads started before the PUT URL lifetime; returns how many went. */
size_t DeleteStalePendingFiles(IBlobClient& blob, const std::string& relay_user_id) {
  auto listed = blob.FileList(relay_user_id, "pending");
  if (!listed) {
    RelayFilesLog().warning << "relay file pending list failed code=" << RelayFileErrorCode(listed.error());
    return 0;
  }
  const std::string cutoff = IsoUtcMs(util::NowUnixMs() - kStalePendingMs);
  size_t deleted = 0;
  for (const RelayFileRecord& file : listed->files) {
    if (file.status != "pending" || file.created_at.empty() || cutoff.empty() || !(file.created_at < cutoff)) {
      continue;
    }
    if (blob.FileDelete(relay_user_id, file.blob_id)) {
      ++deleted;
    }
  }
  RelayFilesLog().info << "relay file stale pending deleted=" << deleted << " listed=" << listed->files.size();
  return deleted;
}

Roe<RelayFileStartResult> StartRelayFile(IBlobClient& blob, const std::string& relay_user_id,
                                         const uint64_t cipher_bytes) {
  auto started = blob.FileStart(relay_user_id, cipher_bytes);
  if (!started && RelayFileErrorCode(started.error()) == "QUOTA_PENDING_COUNT" &&
      DeleteStalePendingFiles(blob, relay_user_id) > 0) {
    started = blob.FileStart(relay_user_id, cipher_bytes);
  }
  return started;
}

/** After start: delete the blob (best effort; its pending slot and reserved bytes go with it), keep `why`. */
Error AbandonRelayFile(IBlobClient& blob, const std::string& relay_user_id, const std::string& blob_id,
                       const char* step, Error why) {
  const std::string code = RelayFileErrorCode(why);
  RelayFilesLog().info << "relay file abandon blob=" << blob_id << " step=" << step
                       << " code=" << (code.empty() ? "none" : code);
  if (auto deleted = blob.FileDelete(relay_user_id, blob_id); !deleted) {
    RelayFilesLog().warning << "relay file abandon delete failed blob=" << blob_id
                            << " code=" << RelayFileErrorCode(deleted.error());
  }
  return why;
}

/** start → `put(upload_url)` → complete, abandoning the blob on any failure after start. */
Roe<RelayFileUpload> UploadRelayFileWith(IBlobClient& blob, const std::string& relay_user_id,
                                         const uint64_t cipher_bytes,
                                         const std::function<Roe<void>(const std::string& upload_url)>& put) {
  if (relay_user_id.empty()) {
    return Error("Register on the network before using relay file storage");
  }
  auto started = StartRelayFile(blob, relay_user_id, cipher_bytes);
  if (!started) {
    return started.error();
  }
  const std::string blob_id = started->blob_id;
  RelayFilesLog().info << "relay file start blob=" << blob_id << " cipher_bytes=" << cipher_bytes
                       << " mode=" << started->mode;
  if (started->mode != "single" || started->upload_url.empty()) {
    return AbandonRelayFile(blob, relay_user_id, blob_id, "start",
                            Error("Relay file upload mode is not supported: " + started->mode));
  }
  if (auto sent = put(started->upload_url); !sent) {
    return AbandonRelayFile(blob, relay_user_id, blob_id, "put", sent.error());
  }
  auto completed = blob.FileComplete(relay_user_id, blob_id);
  if (!completed) {
    return AbandonRelayFile(blob, relay_user_id, blob_id, "complete", completed.error());
  }
  if (completed->share_url.empty()) {
    return AbandonRelayFile(blob, relay_user_id, blob_id, "complete", Error("Relay file complete gave no link"));
  }
  RelayFilesLog().info << "relay file complete blob=" << blob_id;
  return RelayFileUpload{.blob_id = blob_id, .url = completed->share_url};
}

} // namespace

std::string RelayFileErrorCode(const Error& err) {
  // HttpBlobClient's detail: "HTTP <status> <CODE>: <server text>".
  const std::string& text = err.message;
  if (text.rfind("HTTP ", 0) != 0) {
    return {};
  }
  size_t at = 5;
  while (at < text.size() && std::isdigit(static_cast<unsigned char>(text[at]))) {
    ++at;
  }
  if (at >= text.size() || text[at] != ' ') {
    return {};
  }
  const size_t begin = ++at;
  while (at < text.size() && (std::isupper(static_cast<unsigned char>(text[at])) || text[at] == '_')) {
    ++at;
  }
  // Followed by the end, ':' (the server text) or ' ' (a note such as FileStart's " byte_length=N").
  if (at - begin < 3 || (at < text.size() && text[at] != ':' && text[at] != ' ')) {
    return {};
  }
  return text.substr(begin, at - begin);
}

Roe<RelayFileUpload> UploadRelayFile(IBlobClient& blob, const std::string& relay_user_id, const std::string& path,
                                     const uint64_t cipher_bytes,
                                     const std::function<bool(uint64_t, uint64_t)>& progress) {
  return UploadRelayFileWith(blob, relay_user_id, cipher_bytes, [&](const std::string& upload_url) {
    return blob.PutUploadFile(upload_url, "application/octet-stream", path, progress);
  });
}

Roe<RelayFileUpload> UploadRelayFileBytes(IBlobClient& blob, const std::string& relay_user_id,
                                          const std::string& body) {
  return UploadRelayFileWith(blob, relay_user_id, body.size(), [&](const std::string& upload_url) {
    return blob.PutUpload(upload_url, "application/octet-stream", body);
  });
}

std::string BlobPurposeToWire(const BlobPurpose purpose) {
  return purpose == BlobPurpose::Icon ? "icon" : "file";
}

std::optional<BlobPurpose> BlobPurposeFromWire(const std::string& value) {
  if (value == "icon") {
    return BlobPurpose::Icon;
  }
  if (value == "file") {
    return BlobPurpose::File;
  }
  return std::nullopt;
}

std::optional<BlobTier> BlobTierFromWire(const std::string& value) {
  if (value == "small") {
    return BlobTier::Small;
  }
  if (value == "large") {
    return BlobTier::Large;
  }
  return std::nullopt;
}

std::optional<BlobStatus> BlobStatusFromWire(const std::string& value) {
  if (value == "pending") {
    return BlobStatus::Pending;
  }
  if (value == "retained") {
    return BlobStatus::Retained;
  }
  return std::nullopt;
}

Roe<void> IBlobClient::PutUploadFile(const std::string& upload_url, const std::string& content_type,
                                     const std::string& path,
                                     const std::function<bool(uint64_t, uint64_t)>& progress) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    return Error("Upload file is not readable");
  }
  const std::string body((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  if (progress && !progress(0, body.size())) {
    return Error("Upload cancelled");
  }
  auto put = PutUpload(upload_url, content_type, body);
  if (put && progress) {
    (void)progress(body.size(), body.size());
  }
  return put;
}

Roe<BlobPresignResult> UploadRelayBlobFile(IBlobClient& blob, const std::string& relay_user_id,
                                           const std::string& content_type, const BlobPurpose purpose,
                                           const std::string& path, const uint64_t byte_length,
                                           const std::function<bool(uint64_t, uint64_t)>& progress) {
  auto presign = blob.Presign(relay_user_id, content_type, byte_length, purpose);
  if (!presign) {
    return presign.error();
  }
  auto put = blob.PutUploadFile(presign.value().upload_url, content_type, path, progress);
  if (!put) {
    return put.error();
  }
  auto retain = blob.Retain(relay_user_id, presign.value().blob_id);
  if (!retain) {
    return retain.error();
  }
  return presign.value();
}

Roe<BlobPresignResult> UploadRelayBlobBytes(IBlobClient& blob, const std::string& relay_user_id,
                                            const std::string& content_type, const BlobPurpose purpose,
                                            const std::string& body) {
  auto presign = blob.Presign(relay_user_id, content_type, body.size(), purpose);
  if (!presign) {
    return presign.error();
  }
  auto put = blob.PutUpload(presign.value().upload_url, content_type, body);
  if (!put) {
    return put.error();
  }
  auto retain = blob.Retain(relay_user_id, presign.value().blob_id);
  if (!retain) {
    return retain.error();
  }
  return presign.value();
}

} // namespace pbr
