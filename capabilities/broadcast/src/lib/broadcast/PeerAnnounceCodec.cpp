#include "lib/broadcast/PeerAnnounceCodec.h"

#include "lib/broadcast/BroadcastSignedJson.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"
#include "foundation/crypto/SignedBytes.h"
#include "foundation/crypto/SignedReceive.h"
#include "foundation/crypto/TypedBlob.h"

#include "common/ValueJson.h"
#include "mesh/types/MediaChannel.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <sodium.h>
#include <vector>

#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {
namespace {

constexpr char kWhat[] = "peer announce tip";
constexpr SignedVersions kTipVersions{.floor = kPeerAnnounceTipSchemaVersion, .current = kPeerAnnounceTipSchemaVersion};

bool IsVideoLevelValue(const int level) {
  return level >= mesh::kMinVideoLevel && level <= mesh::kMaxVideoLevel;
}

/** Each a real level (1..15): anything else would wrap into a channel that was never announced. */
Roe<std::vector<int>> ReadVideoLevels(const Object& o) {
  std::vector<int> out;
  if (!o.contains("video_levels")) {
    return out;
  }
  const Array* arr = o.getArray("video_levels");
  if (arr == nullptr) {
    return Error("peer announce tip: video_levels is not a list");
  }
  for (const auto& item : arr->elements) {
    const auto* level = std::get_if<int64_t>(&item);
    if (level == nullptr || *level < mesh::kMinVideoLevel || *level > mesh::kMaxVideoLevel) {
      return Error("peer announce tip: a video level is not 1-15");
    }
    out.push_back(static_cast<int>(*level));
  }
  return out;
}

Roe<std::vector<std::string>> ReadL1HopPeerIds(const Object& o) {
  std::vector<std::string> out;
  if (!o.contains("l1_hop_peer_ids")) {
    return out;
  }
  const Array* arr = o.getArray("l1_hop_peer_ids");
  if (arr == nullptr) {
    return Error("peer announce tip: l1_hop_peer_ids is not a list");
  }
  for (const auto& item : arr->elements) {
    auto id = asString(item);
    if (!id) {
      return Error("peer announce tip: an l1_hop_peer_ids entry is not a string");
    }
    out.push_back(*id);
  }
  return out;
}

Value StringsArray(const std::vector<std::string>& ids) {
  std::vector<Value> items;
  items.reserve(ids.size());
  for (const std::string& id : ids) {
    items.emplace_back(id);
  }
  return ArrayValue(std::move(items));
}

Roe<void> CheckTipFields(const PeerAnnounceTip& tip) {
  if (tip.peer_id.empty() || tip.topic_id.empty() || tip.program_id.empty()) {
    return Error("peer announce tip missing peer_id/topic_id/program_id");
  }
  if (!std::all_of(tip.video_levels.begin(), tip.video_levels.end(), IsVideoLevelValue)) {
    return Error("peer announce tip: a video level is not 1-15");
  }
  return {};
}

ByteVector SignBytesOf(const PeerAnnounceTip& tip, const uint8_t version) {
  SignedBytesWriter out(kPeerAnnounceTipDomain, version);
  out.Utf8(tip.peer_id)
      .Utf8(tip.topic_id)
      .Utf8(tip.program_id)
      .Utf8(PeerAnnounceStateToString(tip.state))
      .U64(tip.seq)
      .U64(tip.epoch)
      .I64(tip.created_at_ms)
      .Utf8(tip.join_handle)
      .Utf8(tip.hop_peer_id)
      .U64(tip.l1_hop_peer_ids.size());
  for (const std::string& id : tip.l1_hop_peer_ids) {
    out.Utf8(id);
  }
  out.U64(tip.video_levels.size());
  for (const int level : tip.video_levels) {
    out.U8(static_cast<uint8_t>(level)); // 1..15 (CheckTipFields)
  }
  return out.Utf8(tip.kind)
      .Utf8(tip.viewer_peer_id)
      .Utf8(tip.viewer_msg_id)
      .Utf8(tip.body)
      .Utf8(tip.content_id_hex)
      .Finish(tip.ext);
}

} // namespace

Roe<std::string> MakePeerAnnounceTopicId(const std::string_view peer_id, const std::string_view local_name,
                                         const std::string_view app_ns) {
  if (peer_id.empty() || local_name.empty() || app_ns.empty()) {
    return Error("peer announce topic requires peer_id, app_ns, and local_name");
  }
  EnsureSodiumInit();
  ByteVector material;
  material.reserve(peer_id.size() + app_ns.size() + local_name.size() + 2);
  material.insert(material.end(), peer_id.begin(), peer_id.end());
  material.push_back(0);
  material.insert(material.end(), app_ns.begin(), app_ns.end());
  material.push_back(0);
  material.insert(material.end(), local_name.begin(), local_name.end());

  ByteVector digest(crypto_generichash_BYTES);
  if (crypto_generichash(digest.data(), digest.size(), material.data(), material.size(), nullptr, 0) != 0) {
    return Error("peer announce topic hash failed");
  }
  return BytesToHex(digest);
}

Roe<ByteVector> PeerAnnounceTipSignBytes(const PeerAnnounceTip& tip) {
  if (tip.schema_version < 0 || tip.schema_version > std::numeric_limits<uint8_t>::max()) {
    return Error("peer announce tip schema_version is not a byte");
  }
  if (!std::all_of(tip.video_levels.begin(), tip.video_levels.end(), IsVideoLevelValue)) {
    return Error("peer announce tip: a video level is not 1-15");
  }
  return SignBytesOf(tip, static_cast<uint8_t>(tip.schema_version));
}

Roe<std::string> EncodePeerAnnounceTipJson(const PeerAnnounceTip& tip) {
  Object json;
  json.set("schema_version", static_cast<int64_t>(tip.schema_version));
  json.set("peer_id", tip.peer_id);
  json.set("topic_id", tip.topic_id);
  json.set("program_id", tip.program_id);
  json.set("state", PeerAnnounceStateToString(tip.state));
  json.set("seq", static_cast<int64_t>(tip.seq));
  json.set("epoch", static_cast<int64_t>(tip.epoch));
  json.set("created_at_ms", tip.created_at_ms);
  json.set("join_handle", tip.join_handle);
  // Empty fields are left out: absent reads as empty, which is what is signed.
  if (!tip.hop_peer_id.empty()) {
    json.set("hop_peer_id", tip.hop_peer_id);
  }
  if (!tip.l1_hop_peer_ids.empty()) {
    json.set("l1_hop_peer_ids", StringsArray(tip.l1_hop_peer_ids));
  }
  if (!tip.video_levels.empty()) {
    std::vector<Value> levels;
    for (const int level : tip.video_levels) {
      levels.emplace_back(int64_t{level});
    }
    json.set("video_levels", makeArray(std::move(levels)));
  }
  if (!tip.kind.empty()) {
    json.set("kind", tip.kind);
  }
  if (!tip.viewer_peer_id.empty()) {
    json.set("viewer_peer_id", tip.viewer_peer_id);
  }
  if (!tip.viewer_msg_id.empty()) {
    json.set("viewer_msg_id", tip.viewer_msg_id);
  }
  json.set("body", tip.body);
  json.set("content_id_hex", tip.content_id_hex);
  WriteExtB64(json, tip.ext);
  json.set("signature_b64", tip.signature_b64);
  return DumpJson(json);
}

namespace {

/** Step 2 from the wire: the fields of a tip whose `schema_version` (`version`) is in range. */
Roe<PeerAnnounceTip> ParseTipFields(const Object& o, const int version) {
  PeerAnnounceTip tip;
  tip.schema_version = version;
  std::string state;
  for (auto [key, field] :
       {std::pair{"peer_id", &tip.peer_id}, std::pair{"topic_id", &tip.topic_id},
        std::pair{"program_id", &tip.program_id}, std::pair{"state", &state},
        std::pair{"join_handle", &tip.join_handle}, std::pair{"hop_peer_id", &tip.hop_peer_id},
        std::pair{"kind", &tip.kind}, std::pair{"viewer_peer_id", &tip.viewer_peer_id},
        std::pair{"viewer_msg_id", &tip.viewer_msg_id}, std::pair{"body", &tip.body},
        std::pair{"content_id_hex", &tip.content_id_hex}, std::pair{"signature_b64", &tip.signature_b64}}) {
    auto value = ReadOptionalString(o, key, kWhat);
    if (!value) {
      return value.error();
    }
    *field = std::move(*value);
  }
  const auto parsed_state = PeerAnnounceStateFromString(state);
  if (!parsed_state) {
    return Error("invalid peer announce state");
  }
  tip.state = *parsed_state;
  for (auto [key, field] : {std::pair{"seq", &tip.seq}, std::pair{"epoch", &tip.epoch}}) {
    if (!o.contains(key)) {
      continue;
    }
    const auto value = ObjectNonNegInt(o, key);
    if (!value) {
      return Error(std::string(kWhat) + ": " + key + " is not a non-negative integer");
    }
    *field = *value;
  }
  auto created = ReadOptionalInt(o, "created_at_ms", 0, kWhat);
  if (!created) {
    return created.error();
  }
  tip.created_at_ms = *created;
  auto l1 = ReadL1HopPeerIds(o);
  if (!l1) {
    return l1.error();
  }
  tip.l1_hop_peer_ids = std::move(*l1);
  auto levels = ReadVideoLevels(o);
  if (!levels) {
    return levels.error();
  }
  tip.video_levels = std::move(*levels);
  auto ext = ReadExtB64(o);
  if (!ext) {
    return Error(std::string(kWhat) + ": " + ext.error().message);
  }
  tip.ext = std::move(*ext);
  if (auto fields = CheckTipFields(tip); !fields) {
    return fields.error();
  }
  return tip;
}

/** Steps 3–5 for a parsed tip: the signature's algorithm id, the publisher's key, the signature, the area. */
struct TipReceive {
  const PeerAnnounceTip* tip = nullptr;
  TypedRead publisher_key;
  TypedRead signature;

  Roe<void> Algorithms() {
    signature = ReadTypedBlobB64(pp::BlobKind::kSignature, tip->signature_b64, "peer announce tip signature");
    if (!signature.ok()) {
      return signature.ToError();
    }
    if (!publisher_key.ok()) {
      return publisher_key.ToError();
    }
    return {};
  }

  Roe<void> Verify() const {
    return VerifyTyped(publisher_key, SignBytesOf(*tip, static_cast<uint8_t>(tip->schema_version)), signature);
  }
};

} // namespace

Roe<PeerAnnounceTip> DecodePeerAnnounceTipJson(const std::string_view json) {
  auto parsed = ParseObject(std::string(json));
  if (!parsed) {
    return parsed.error();
  }
  // 1. The version, before any field is read; 2. the fields.
  auto version = ReadSchemaVersion(*parsed, kWhat, kPeerAnnounceTipSchemaVersion);
  if (!version) {
    return version.error();
  }
  return ParseTipFields(*parsed, *version);
}

SignedReceipt ReceivePeerAnnounceTipJson(const std::string_view json, const std::vector<uint8_t>& mldsa_public_key,
                                         PeerAnnounceTip* out) {
  auto parsed = ParseObject(std::string(json));
  if (!parsed) {
    return SignedReceipt{.verdict = SignedVerdict::Malformed, .version = 0, .detail = parsed.error().message};
  }
  PeerAnnounceTip tip;
  TipReceive receive{.tip = &tip, .publisher_key = PublisherSigningKey(mldsa_public_key)};
  const std::optional<int64_t> version = OutsideVersionOfJson(*parsed, "schema_version");
  SignedReceipt receipt = ReceiveSigned({
      .what = kWhat,
      .outside_version = version,
      .versions = kTipVersions,
      .parse = [&]() -> Roe<void> {
        auto fields = ParseTipFields(*parsed, static_cast<int>(*version));
        if (!fields) {
          return fields.error();
        }
        tip = std::move(*fields);
        return {};
      },
      .algorithms = [&] { return receive.Algorithms(); },
      .verify = [&] { return receive.Verify(); },
      .ext = &tip.ext,
  });
  if (receipt.accepted() && out != nullptr) {
    *out = std::move(tip);
  }
  return receipt;
}

Roe<PeerAnnounceTip> SignPeerAnnounceTip(PeerAnnounceTip tip, const std::vector<uint8_t>& mldsa_secret_key) {
  if (mldsa_secret_key.size() != kMlDsa65SecretKeyBytes) {
    return Error("peer announce sign requires ML-DSA-65 secret key");
  }
  auto sign_bytes = PeerAnnounceTipSignBytes(tip);
  if (!sign_bytes) {
    return sign_bytes.error();
  }
  auto signature = SignTyped(mldsa_secret_key, *sign_bytes);
  if (!signature) {
    return signature.error();
  }
  tip.signature_b64 = Base64Encode(*signature);
  return tip;
}

SignedReceipt ReceivePeerAnnounceTip(const PeerAnnounceTip& tip, const std::vector<uint8_t>& mldsa_public_key) {
  TipReceive receive{.tip = &tip, .publisher_key = PublisherSigningKey(mldsa_public_key)};
  return ReceiveSigned({
      .what = kWhat,
      .outside_version = tip.schema_version,
      .versions = kTipVersions,
      .parse = [&] { return CheckTipFields(tip); },
      .algorithms = [&] { return receive.Algorithms(); },
      .verify = [&] { return receive.Verify(); },
      .ext = &tip.ext,
  });
}

Roe<void> VerifyPeerAnnounceTip(const PeerAnnounceTip& tip, const std::vector<uint8_t>& mldsa_public_key) {
  return ReceivePeerAnnounceTip(tip, mldsa_public_key).ToRoe();
}

int64_t PeerAnnounceNextHeartbeatAtMs(const int64_t last_emit_ms, const double jitter_unit) {
  const double u = std::clamp(jitter_unit, 0.0, 1.0);
  const double span =
      static_cast<double>(kPeerAnnounceLiveHeartbeatMaxIntervalMs - kPeerAnnounceLiveHeartbeatMinIntervalMs);
  const auto interval = kPeerAnnounceLiveHeartbeatMinIntervalMs + static_cast<int64_t>(std::llround(u * span));
  return last_emit_ms + interval;
}

bool PeerAnnounceHeartbeatDue(const int64_t last_emit_ms, const int64_t now_ms) {
  if (last_emit_ms <= 0) {
    return true;
  }
  return now_ms - last_emit_ms >= kPeerAnnounceLiveHeartbeatMinIntervalMs;
}

} // namespace pbr
