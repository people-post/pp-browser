#include "base/messaging/PskRotateCodec.h"

#include "common/ValueJson.h"
#include "common/PbrCompat.h"

namespace pbr {

namespace {

Roe<PskRotateDetail> ParseDetailJson(const std::string& detail_json) {
  auto json = TryParseObject(detail_json);
  if (!json) {
    return Error("psk_rotate detail must be a JSON object");
  }
  PskRotateDetail detail;
  detail.rotation_id = json->getString("rotation_id").value_or(std::string{});
  detail.new_epoch = static_cast<uint32_t>(json->getNonNegInt("new_epoch").value_or(0));
  detail.wrap_kind = json->getString("wrap_kind").value_or(std::string{});
  detail.thread_kem_pk_b64 = json->getString("thread_kem_pk_b64").value_or(std::string{});
  detail.key_init_hash = json->getString("key_init_hash").value_or(std::string{});
  return detail;
}

} // namespace

bool PskRotateCodec::IsPskRotateMessage(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::System || message.payload_json.empty()) {
    return false;
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return false;
  }
  auto control_type = payload->getString("control_type");
  return control_type && *control_type == kPskRotateControlType;
}

Roe<PskRotateDetail> PskRotateCodec::Decode(const ThreadMessage& message) {
  if (!IsPskRotateMessage(message)) {
    return Error("not a psk_rotate message");
  }
  auto payload = TryParseObject(message.payload_json);
  if (!payload) {
    return Error("psk_rotate missing detail");
  }
  if (!payload->contains("detail")) {
    return Error("psk_rotate missing detail");
  }
  auto detail_json = payload->getString("detail");
  if (!detail_json) {
    return Error("psk_rotate detail must be a JSON string");
  }
  auto parsed = ParseDetailJson(*detail_json);
  if (!parsed) {
    return parsed.error();
  }
  if (auto valid = Validate(*parsed); !valid) {
    return valid.error();
  }
  return parsed;
}

Roe<std::string> PskRotateCodec::EncodePayloadJson(const PskRotateDetail& detail) {
  if (auto valid = Validate(detail); !valid) {
    return valid.error();
  }
  Object detail_json;
  detail_json.set("rotation_id", detail.rotation_id);
  detail_json.setJsonUInt("new_epoch", detail.new_epoch);
  detail_json.set("wrap_kind", detail.wrap_kind);
  detail_json.set("thread_kem_pk_b64", detail.thread_kem_pk_b64);
  detail_json.set("key_init_hash", detail.key_init_hash);
  Object payload;
  payload.set("control_type", kPskRotateControlType);
  payload.set("detail", DumpJson(detail_json));
  return DumpJson(payload);
}

Roe<void> PskRotateCodec::Validate(const PskRotateDetail& detail) {
  if (detail.rotation_id.empty()) {
    return Error("psk_rotate missing rotation_id");
  }
  if (detail.new_epoch < 2) {
    return Error("psk_rotate new_epoch must be >= 2");
  }
  if (detail.wrap_kind != kPskRotateWrapAccountKem && detail.wrap_kind != kPskRotateWrapThreadKem) {
    return Error("psk_rotate wrap_kind must be account_kem or thread_kem");
  }
  if (detail.thread_kem_pk_b64.empty()) {
    return Error("psk_rotate missing thread_kem_pk_b64");
  }
  if (detail.key_init_hash.size() != 64) {
    return Error("psk_rotate key_init_hash must be 32-byte hex");
  }
  return {};
}

bool PskRotateCodec::RotationIdWins(const std::string& left, const std::string& right) {
  return left > right;
}

} // namespace pbr
