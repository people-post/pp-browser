#include "domain/messaging/PeerAnnounceCodec.h"

#include "foundation/crypto/CryptoUtil.h"
#include "foundation/crypto/MlDsa.h"

#include "common/ValueJson.h"

#include <algorithm>
#include <cmath>
#include <sodium.h>
#include <sstream>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

void AppendField(std::ostringstream& out, const char* key, const std::string& value) {
  out << key << '=' << value << '\n';
}
void AppendField(std::ostringstream& out, const char* key, const int64_t value) {
  out << key << '=' << value << '\n';
}
void AppendField(std::ostringstream& out, const char* key, const uint64_t value) {
  out << key << '=' << value << '\n';
}

ByteVector ToBytes(const std::string& text) {
  return ByteVector(text.begin(), text.end());
}

std::string JoinL1HopPeerIds(const std::vector<std::string>& ids) {
  std::string out;
  for (const std::string& id : ids) {
    if (id.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(',');
    }
    out += id;
  }
  return out;
}

std::vector<std::string> ReadL1HopPeerIds(const Object& o) {
  std::vector<std::string> out;
  if (const Array* arr = o.getArray("l1_hop_peer_ids")) {
    for (const auto& item : arr->elements) {
      if (auto s = asString(item)) {
        if (!s->empty()) {
          out.push_back(*s);
        }
      }
    }
  }
  return out;
}

Value L1HopPeerIdsArray(const std::vector<std::string>& ids) {
  std::vector<Value> items;
  items.reserve(ids.size());
  for (const std::string& id : ids) {
    if (!id.empty()) {
      items.emplace_back(id);
    }
  }
  return ArrayValue(std::move(items));
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

std::string PeerAnnounceCanonicalSignBytes(const PeerAnnounceTip& tip) {
  std::ostringstream out;
  AppendField(out, "v", static_cast<int64_t>(tip.schema_version));
  AppendField(out, "peer_id", tip.peer_id);
  AppendField(out, "topic_id", tip.topic_id);
  AppendField(out, "program_id", tip.program_id);
  AppendField(out, "state", std::string(PeerAnnounceStateToString(tip.state)));
  AppendField(out, "seq", tip.seq);
  AppendField(out, "epoch", tip.epoch);
  AppendField(out, "created_at_ms", tip.created_at_ms);
  AppendField(out, "join_handle", tip.join_handle);
  // Additive: omit when empty so pre-hop tips keep verifying.
  if (!tip.hop_peer_id.empty()) {
    AppendField(out, "hop_peer_id", tip.hop_peer_id);
  }
  // Additive B007 L1 list (omit empty).
  if (const std::string joined = JoinL1HopPeerIds(tip.l1_hop_peer_ids); !joined.empty()) {
    AppendField(out, "l1_hop_peer_ids", joined);
  }
  // Additive kind / viewer attribution (omit defaults for legacy verify).
  if (!tip.kind.empty()) {
    AppendField(out, "kind", tip.kind);
  }
  if (!tip.viewer_peer_id.empty()) {
    AppendField(out, "viewer_peer_id", tip.viewer_peer_id);
  }
  if (!tip.viewer_msg_id.empty()) {
    AppendField(out, "viewer_msg_id", tip.viewer_msg_id);
  }
  AppendField(out, "body", tip.body);
  AppendField(out, "content_id_hex", tip.content_id_hex);
  return out.str();
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
  if (!tip.hop_peer_id.empty()) {
    json.set("hop_peer_id", tip.hop_peer_id);
  }
  if (!tip.l1_hop_peer_ids.empty()) {
    json.set("l1_hop_peer_ids", L1HopPeerIdsArray(tip.l1_hop_peer_ids));
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
  json.set("signature_b64", tip.signature_b64);
  return DumpJson(json);
}

Roe<PeerAnnounceTip> DecodePeerAnnounceTipJson(const std::string_view json) {
  auto parsed = ParseObject(std::string(json));
  if (!parsed) {
    return parsed.error();
  }
  const Object& o = *parsed;
  PeerAnnounceTip tip;
  tip.schema_version =
      static_cast<int>(ObjectInt64(o, "schema_version").value_or(kPeerAnnounceTipSchemaVersion));
  if (tip.schema_version != kPeerAnnounceTipSchemaVersion) {
    return Error("unsupported peer announce schema_version");
  }
  tip.peer_id = ObjectString(o, "peer_id").value_or("");
  tip.topic_id = ObjectString(o, "topic_id").value_or("");
  tip.program_id = ObjectString(o, "program_id").value_or("");
  const auto state = PeerAnnounceStateFromString(ObjectString(o, "state").value_or(""));
  if (!state) {
    return Error("invalid peer announce state");
  }
  tip.state = *state;
  tip.seq = ObjectNonNegInt(o, "seq").value_or(0);
  tip.epoch = ObjectNonNegInt(o, "epoch").value_or(0);
  tip.created_at_ms = ObjectInt64(o, "created_at_ms").value_or(0);
  tip.join_handle = ObjectString(o, "join_handle").value_or("");
  tip.hop_peer_id = ObjectString(o, "hop_peer_id").value_or("");
  tip.l1_hop_peer_ids = ReadL1HopPeerIds(o);
  tip.kind = ObjectString(o, "kind").value_or("");
  tip.viewer_peer_id = ObjectString(o, "viewer_peer_id").value_or("");
  tip.viewer_msg_id = ObjectString(o, "viewer_msg_id").value_or("");
  tip.body = ObjectString(o, "body").value_or("");
  tip.content_id_hex = ObjectString(o, "content_id_hex").value_or("");
  tip.signature_b64 = ObjectString(o, "signature_b64").value_or("");
  if (tip.peer_id.empty() || tip.topic_id.empty() || tip.program_id.empty()) {
    return Error("peer announce tip missing peer_id/topic_id/program_id");
  }
  return tip;
}

Roe<PeerAnnounceTip> SignPeerAnnounceTip(PeerAnnounceTip tip, const std::vector<uint8_t>& mldsa_secret_key) {
  if (mldsa_secret_key.size() != kMlDsa65SecretKeyBytes) {
    return Error("peer announce sign requires ML-DSA-65 secret key");
  }
  const auto canonical = ToBytes(PeerAnnounceCanonicalSignBytes(tip));
  auto signature = MlDsa::Sign(mldsa_secret_key, canonical);
  if (!signature) {
    return signature.error();
  }
  tip.signature_b64 = Base64Encode(*signature);
  return tip;
}

Roe<void> VerifyPeerAnnounceTip(const PeerAnnounceTip& tip, const std::vector<uint8_t>& mldsa_public_key) {
  if (mldsa_public_key.size() != kMlDsa65PublicKeyBytes) {
    return Error("peer announce verify requires ML-DSA-65 public key");
  }
  if (tip.signature_b64.empty()) {
    return Error("peer announce tip missing signature");
  }
  auto signature = Base64Decode(tip.signature_b64);
  if (!signature) {
    return signature.error();
  }
  if (signature->size() != kMlDsa65SignatureBytes) {
    return Error("peer announce signature size invalid");
  }
  const auto canonical = ToBytes(PeerAnnounceCanonicalSignBytes(tip));
  auto ok = MlDsa::Verify(mldsa_public_key, canonical, *signature);
  if (!ok) {
    return ok.error();
  }
  if (!*ok) {
    return Error("peer announce signature verify failed");
  }
  return {};
}

int64_t PeerAnnounceNextHeartbeatAtMs(const int64_t last_emit_ms, const double jitter_unit) {
  const double u = std::clamp(jitter_unit, 0.0, 1.0);
  const double span = static_cast<double>(kPeerAnnounceLiveHeartbeatMaxIntervalMs -
                                          kPeerAnnounceLiveHeartbeatMinIntervalMs);
  const auto interval =
      kPeerAnnounceLiveHeartbeatMinIntervalMs + static_cast<int64_t>(std::llround(u * span));
  return last_emit_ms + interval;
}

bool PeerAnnounceHeartbeatDue(const int64_t last_emit_ms, const int64_t now_ms) {
  if (last_emit_ms <= 0) {
    return true;
  }
  return now_ms - last_emit_ms >= kPeerAnnounceLiveHeartbeatMinIntervalMs;
}

} // namespace pbr
