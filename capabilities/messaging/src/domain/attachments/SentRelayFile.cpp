#include "domain/attachments/SentRelayFile.h"

#include "domain/attachments/RelayFileUrl.h"
#include "lib/delivery/ChatPayloadCodec.h"

#include <algorithm>

namespace pbr {

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
  if (now_ms - sent_at_ms > kRelayFileLifetimeMs) {
    return true;
  }
  if (our_files == nullptr || our_files->files.size() >= kRelayFileListCap) {
    return false;
  }
  return std::none_of(our_files->files.begin(), our_files->files.end(),
                      [&blob_id](const RelayFileRecord& file) { return file.blob_id == blob_id; });
}

} // namespace pbr
