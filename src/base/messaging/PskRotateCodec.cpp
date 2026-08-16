#include "base/messaging/PskRotateCodec.h"

#include <nlohmann/json.hpp>

namespace pbr {

namespace {

Roe<PskRotateDetail> ParseDetailJson(const std::string& detail_json) {
  const nlohmann::json json = nlohmann::json::parse(detail_json, nullptr, false);
  if (!json.is_object()) {
    return Error("psk_rotate detail must be a JSON object");
  }
  PskRotateDetail detail;
  detail.rotation_id = json.value("rotation_id", std::string{});
  detail.new_epoch = json.value("new_epoch", 0u);
  detail.wrap_kind = json.value("wrap_kind", std::string{});
  detail.thread_kem_pk_b64 = json.value("thread_kem_pk_b64", std::string{});
  detail.key_init_hash = json.value("key_init_hash", std::string{});
  return detail;
}

} // namespace

bool PskRotateCodec::IsPskRotateMessage(const ThreadMessage& message) {
  if (message.content_type != ChatContentType::System || message.payload_json.empty()) {
    return false;
  }
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.is_object() || !payload.contains("control_type") || !payload["control_type"].is_string()) {
    return false;
  }
  return payload["control_type"].get<std::string>() == kPskRotateControlType;
}

Roe<PskRotateDetail> PskRotateCodec::Decode(const ThreadMessage& message) {
  if (!IsPskRotateMessage(message)) {
    return Error("not a psk_rotate message");
  }
  const nlohmann::json payload = nlohmann::json::parse(message.payload_json, nullptr, false);
  if (!payload.contains("detail")) {
    return Error("psk_rotate missing detail");
  }
  if (!payload["detail"].is_string()) {
    return Error("psk_rotate detail must be a JSON string");
  }
  const std::string detail_json = payload["detail"].get<std::string>();
  auto parsed = ParseDetailJson(detail_json);
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
  const nlohmann::json detail_json = {{"rotation_id", detail.rotation_id},
                                      {"new_epoch", detail.new_epoch},
                                      {"wrap_kind", detail.wrap_kind},
                                      {"thread_kem_pk_b64", detail.thread_kem_pk_b64},
                                      {"key_init_hash", detail.key_init_hash}};
  return nlohmann::json({{"control_type", kPskRotateControlType}, {"detail", detail_json.dump()}}).dump();
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
