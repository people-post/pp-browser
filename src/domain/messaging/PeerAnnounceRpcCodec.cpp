#include "domain/messaging/PeerAnnounceRpcCodec.h"

#include "domain/messaging/PeerAnnounceCodec.h"

#include "common/ValueJson.h"

#include "common/PbrCompat.h"

namespace pbr {

Roe<std::string> EncodePeerAnnounceTipPush(const PeerAnnounceTip& tip) {
  auto tip_json = EncodePeerAnnounceTipJson(tip);
  if (!tip_json) {
    return tip_json.error();
  }
  auto tip_obj = ParseObject(*tip_json);
  if (!tip_obj) {
    return tip_obj.error();
  }
  Object root;
  root.set("op", kPeerAnnounceOpTipPush);
  root.set("tip", *tip_obj);
  return DumpJson(root);
}

Roe<std::string> EncodePeerAnnounceTipAck(const PeerAnnounceTipAck& ack) {
  Object root;
  root.set("op", kPeerAnnounceOpTipAck);
  root.set("ok", ack.ok);
  if (!ack.error.empty()) {
    root.set("error", ack.error);
  }
  root.set("seq", static_cast<int64_t>(ack.seq));
  root.set("epoch", static_cast<int64_t>(ack.epoch));
  return DumpJson(root);
}

Roe<PeerAnnounceRpcMessage> DecodePeerAnnounceRpcJson(const std::string_view json) {
  auto parsed = ParseObject(std::string(json));
  if (!parsed) {
    return parsed.error();
  }
  const Object& root = *parsed;
  const std::string op = ObjectString(root, "op").value_or("");
  if (op == kPeerAnnounceOpTipPush) {
    const Object* tip_obj = ObjectChild(root, "tip");
    if (tip_obj == nullptr) {
      return Error("peer announce tip_push missing tip object");
    }
    auto tip = DecodePeerAnnounceTipJson(DumpJson(*tip_obj));
    if (!tip) {
      return tip.error();
    }
    return PeerAnnounceRpcMessage{PeerAnnounceTipPush{std::move(*tip)}};
  }
  if (op == kPeerAnnounceOpTipAck) {
    PeerAnnounceTipAck ack;
    ack.ok = ObjectBool(root, "ok").value_or(false);
    ack.error = ObjectString(root, "error").value_or("");
    ack.seq = ObjectNonNegInt(root, "seq").value_or(0);
    ack.epoch = ObjectNonNegInt(root, "epoch").value_or(0);
    return PeerAnnounceRpcMessage{std::move(ack)};
  }
  return Error("unsupported peer announce rpc op");
}

} // namespace pbr
