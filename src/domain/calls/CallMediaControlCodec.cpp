#include "domain/calls/CallMediaControlCodec.h"

#include "common/ValueJson.h"

#include "common/PbrCompat.h"

namespace pbr {
namespace {

CallMediaControlType TypeFromWire(const std::string& type) {
  if (type == "hello") {
    return CallMediaControlType::Hello;
  }
  if (type == "hello_ack") {
    return CallMediaControlType::HelloAck;
  }
  if (type == "hb") {
    return CallMediaControlType::Heartbeat;
  }
  if (type == "migrate") {
    return CallMediaControlType::Migrate;
  }
  if (type == "path_add") {
    return CallMediaControlType::PathAdd;
  }
  if (type == "migrate_ack") {
    return CallMediaControlType::MigrateAck;
  }
  if (type == "path_release") {
    return CallMediaControlType::PathRelease;
  }
  if (type == "path_release_ack") {
    return CallMediaControlType::PathReleaseAck;
  }
  return CallMediaControlType::Unknown;
}

} // namespace

std::optional<CallMediaControl> DecodeCallMediaControl(const std::string& json) {
  auto root = TryParseObject(json);
  if (!root) {
    return std::nullopt;
  }
  CallMediaControl control;
  control.type = TypeFromWire(root->getString("type").value_or(""));
  control.call_id = root->getString("call_id").value_or("");
  const uint32_t epoch_default = control.type == CallMediaControlType::Hello ? 1 : 0;
  control.media_epoch = static_cast<uint32_t>(root->getNonNegInt("media_epoch").value_or(epoch_default));
  control.path_gen = static_cast<uint32_t>(root->getNonNegInt("path_gen").value_or(0));
  control.sender_offerer = root->getString("role").value_or("") == "offerer";
  control.ok = root->getIf<bool>("ok").value_or(false);
  control.error = root->getString("error").value_or("");
  control.active = root->getIf<bool>("active").value_or(false);
  return control;
}

std::string EncodeCallMediaHello(const std::string& call_id, const uint32_t media_epoch, const bool offerer) {
  Object hello;
  hello.set("v", int64_t{1});
  hello.set("type", "hello");
  hello.set("call_id", call_id);
  hello.setJsonUInt("media_epoch", media_epoch);
  hello.set("role", offerer ? "offerer" : "answerer");
  return DumpJson(hello);
}

std::string EncodeCallMediaHelloAck(const bool ok, const char* error) {
  Object ack;
  ack.set("v", int64_t{1});
  ack.set("type", "hello_ack");
  ack.set("ok", ok);
  if (!ok && error) {
    ack.set("error", error);
  }
  return DumpJson(ack);
}

const std::string& EncodeCallMediaHeartbeat(const bool active) {
  static const std::string on_active = R"({"v":1,"type":"hb","active":true})";
  static const std::string on_standby = R"({"v":1,"type":"hb"})";
  return active ? on_active : on_standby;
}

std::string EncodeCallMediaMigrate(const std::string& call_id, const uint32_t media_epoch, const uint32_t path_gen,
                                   const bool standby_only) {
  Object o;
  o.set("v", static_cast<int64_t>(1));
  o.set("type", standby_only ? "path_add" : "migrate");
  o.set("call_id", call_id);
  o.set("media_epoch", static_cast<int64_t>(media_epoch));
  o.set("path_gen", static_cast<int64_t>(path_gen));
  return DumpJson(o);
}

std::string EncodeCallMediaMigrateAck(const bool ok, const uint32_t path_gen, const std::string& error) {
  Object o;
  o.set("v", static_cast<int64_t>(1));
  o.set("type", "migrate_ack");
  o.set("ok", ok);
  o.set("path_gen", static_cast<int64_t>(path_gen));
  if (!error.empty()) {
    o.set("error", error);
  }
  return DumpJson(o);
}

std::string EncodeCallMediaPathRelease(const bool ack, const uint32_t path_gen) {
  Object o;
  o.set("v", static_cast<int64_t>(1));
  o.set("type", ack ? "path_release_ack" : "path_release");
  o.set("path_gen", static_cast<int64_t>(path_gen));
  return DumpJson(o);
}

} // namespace pbr
