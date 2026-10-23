#pragma once

#include "foundation/crypto/CryptoUtil.h"
#include "common/ValueJson.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/**
 * Where the image of a sent AI question lives (private CAS, like a chat attachment) and how big it is.
 * Stored as `ThreadMessage::ai_image_json` inside the message's encrypted transcript envelope.
 */
struct AiImageRef {
  std::vector<uint8_t> content_hash; // 32 bytes; the CAS object id
  std::string mime;
  int width = 0; // pixels
  int height = 0;
};

inline std::string AiImageRefToJson(const AiImageRef& ref) {
  Object json;
  json.set("content_hash", BytesToHex(ref.content_hash));
  json.set("mime", ref.mime);
  json.set("width", static_cast<int64_t>(ref.width));
  json.set("height", static_cast<int64_t>(ref.height));
  return DumpJson(json);
}

/** nullopt unless the JSON names a 32-byte hash and a mime; width/height default to 0 (no explicit size). */
inline std::optional<AiImageRef> AiImageRefFromJson(const std::string& json) {
  const auto parsed = TryParseObject(json);
  if (!parsed) {
    return std::nullopt;
  }
  const auto hex = parsed->getString("content_hash");
  const auto mime = parsed->getString("mime");
  if (!hex || !mime || mime->empty()) {
    return std::nullopt;
  }
  auto hash = HexToBytes(*hex);
  if (!hash || hash->size() != 32) {
    return std::nullopt;
  }
  AiImageRef ref;
  ref.content_hash = std::move(*hash);
  ref.mime = *mime;
  if (const auto w = parsed->getNonNegInt("width")) {
    ref.width = static_cast<int>(*w);
  }
  if (const auto h = parsed->getNonNegInt("height")) {
    ref.height = static_cast<int>(*h);
  }
  return ref;
}

} // namespace pbr
