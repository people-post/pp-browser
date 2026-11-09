#include "feature/calls/CallControlClient.h"
#include "lib/calls/CallSessionLogic.h"

#include "common/Utilities.h"
#include "common/ValueJson.h"
#include "common/directory/DirectoryJson.h"
#include "lib/calls/CallMediaKeyExchange.h"
#include "lib/calls/ICallMediaKeyStore.h"
#include "lib/delivery/SendRelayOptions.h"
#include "foundation/crypto/CryptoUtil.h"

#include <utility>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

CallControlClient::CallControlClient(IThreadTranscript& transcript, const ILocalAccount& account,
                                     const ICallSessionReader& sessions, const CallDeliveryPorts& delivery)
    : transcript_(transcript), account_(account), sessions_(sessions), delivery_(delivery) {
  redirectLogger("CallControlClient");
}

Roe<std::string> CallControlClient::LocalRelayIdentity() const {
  auto account = account_.GetAccountId();
  if (!account || account->empty()) {
    return Error("Local account identity unavailable");
  }
  return *account;
}

Roe<void> CallControlClient::SendDirect(const std::string& peer_identity, const CallControlType type,
                                        const std::string& detail_json, const std::string& display,
                                        const bool urgent_allowed) {
  if (!delivery_.send_control) {
    return Error("Call delivery not bound");
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto control = CallControlCodec::BuildSystemMessage("", type, display, detail_json, *local);
  if (!control) {
    return control.error();
  }
  // A call invite rings: the relay wakes the peer as for an incoming call (P012).
  const bool urgent = urgent_allowed && type == CallControlType::CallInvite;
  return delivery_.send_control(peer_identity, *control, urgent);
}

Roe<void> CallControlClient::SendSummary(const std::string& thread_id, const CallSummaryDetail& summary_in,
                                         const bool local_only, const SummaryWrite write) {
  CallSummaryDetail summary = summary_in;
  // A group call's record is one fixed-id row this device rewrites (left, rejoined, call ended).
  std::string message_id;
  if (summary.group && local_only) {
    message_id = GroupCallSummaryMessageId(summary.call_id);
    std::optional<CallSummaryDetail> existing;
    auto row = transcript_.GetMessage(thread_id, message_id);
    if (!row) {
      return row.error();  // could not read it: do not write over a record that may be there
    }
    if (row->has_value()) {
      auto payload = TryParseObject((*row)->payload_json);
      auto decoded = payload ? CallControlCodec::DecodeSummary(payload->getString("detail").value_or(""))
                             : Roe<CallSummaryDetail>(Error("call record unreadable"));
      if (!decoded) {
        return decoded.error();
      }
      existing = *decoded;
    }
    auto merged = MergeGroupSummary(existing, summary, write);
    if (!merged) {
      return {};
    }
    summary = *merged;
  }
  auto detail = CallControlCodec::EncodeSummary(summary);
  if (!detail) {
    return detail.error();
  }
  // The transcript shows the family's line; the text stands in where no line is drawn (previews).
  const std::string text = summary.media_mode == CallMediaMode::Video ? "Video call" : "Voice call";
  if (delivery_.send_to_thread && !local_only) {
    SendRelayOptions opts;
    opts.content_type = ChatContentType::System;
    Object payload;
    payload.set("control_type", CallControlTypeToWire(CallControlType::CallSummary));
    payload.set("detail", *detail);
    opts.payload_json = DumpJson(payload);
    opts.generation = "system";
    if (auto sent = delivery_.send_to_thread(thread_id, text, opts); sent) {
      return {};
    } else {
      log().info << "call summary kept local thread=" << thread_id << " (" << sent.error().message << ")";
    }
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    return local.error();
  }
  auto message =
      CallControlCodec::BuildSystemMessage(thread_id, CallControlType::CallSummary, text, *detail, *local, message_id);
  if (!message) {
    return message.error();
  }
  message->sender_contact_id = kLocalSelfContactId;
  // Upsert, not get-then-write: the leave, the call's end and a restart cleanup may write at once.
  if (auto appended = message_id.empty() ? transcript_.AppendMessage(*message) : transcript_.UpsertMessage(*message);
      !appended) {
    return appended.error();
  }
  return {};
}

void CallControlClient::SendToEach(const std::vector<std::string>& peers, const CallControlType type,
                                   const std::string& detail_json, const std::string& display) {
  if (peers.empty()) {
    return;
  }
  if (!delivery_.send_control_to_many) {
    log().warning << "send to each: call delivery not bound type=" << CallControlTypeToWire(type);
    return;
  }
  auto local = LocalRelayIdentity();
  if (!local) {
    log().warning << "send to each: " << local.error().message;
    return;
  }
  // A message per peer: each copy has its own id (the outbox's retries and the receiver's dedupe go by it).
  std::vector<ControlSend> sends;
  sends.reserve(peers.size());
  for (const std::string& peer : peers) {
    auto control = CallControlCodec::BuildSystemMessage("", type, display, detail_json, *local);
    if (!control) {
      log().warning << "send to " << peer << ": " << control.error().message;
      continue;
    }
    sends.push_back(ControlSend{
        .peer_identity = peer, .control = std::move(*control), .urgent = type == CallControlType::CallInvite});
  }
  // Reported on a worker, maybe after this client is gone: it logs and touches nothing of ours.
  delivery_.send_control_to_many(std::move(sends), [type](std::vector<ControlSendResult> results) {
    auto logger = logging::getLogger("CallControlClient");
    size_t queued = 0;
    for (const ControlSendResult& result : results) {
      if (result.queued) {
        ++queued;
      } else {
        logger.warning << "send to " << result.peer_identity << " failed type=" << CallControlTypeToWire(type)
                       << " err=" << result.queued.error().message;
      }
    }
    logger.info << "sent to each queued=" << queued << "/" << results.size() << " type=" << CallControlTypeToWire(type);
  });
}

Roe<void> CallControlClient::FanOutToJoined(const std::string& call_id, const CallControlType type,
                                             const std::string& detail_json, const std::string& display,
                                             const std::string& skip_identity) {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  // Best-effort: one peer failure must not block CallSfuAttach / roster to the rest.
  SendToEach(SelectCallFanoutIdentities(*participants, skip_identity, true, false), type, detail_json, display);
  return {};
}

Roe<void> CallControlClient::FanOutToJoinedAndRinging(const std::string& call_id, const CallControlType type,
                                                       const std::string& detail_json, const std::string& display,
                                                       const std::string& skip_identity) {
  auto participants = sessions_.ListParticipants(call_id);
  if (!participants) {
    return participants.error();
  }
  SendToEach(SelectCallFanoutIdentities(*participants, skip_identity, true, true), type, detail_json, display);
  return {};
}

Roe<ByteVector> CallControlClient::SessionKeyWith(const std::string& peer_identity) const {
  if (!delivery_.ensure_peer_session_key) {
    return Error("Call delivery not bound");
  }
  return delivery_.ensure_peer_session_key(peer_identity);
}

Roe<ByteVector> CallControlClient::HeldSessionKeyWith(const std::string& peer_identity) const {
  if (!delivery_.held_peer_session_key) {
    return Error("Call delivery not bound");
  }
  return delivery_.held_peer_session_key(peer_identity);
}

Roe<void> CallControlClient::SendMediaKey(const std::string& call_id, const std::string& peer_identity,
                                          const uint32_t media_epoch, const std::string& media_key_id,
                                          const ByteVector& key_bytes) {
  CallMediaKeyDetail key_detail;
  key_detail.call_id = call_id;
  key_detail.media_epoch = media_epoch;
  key_detail.media_key_id = media_key_id;
  // The key itself: the lane copy each of the peer's devices reads is sealed in that device's session (M024).
  key_detail.media_key_b64 = Base64Encode(key_bytes);
  if (kMediaKeyAccountWrapBridge) {
    // The old form beside it, for an app before M024: wrapped under the account pair's session. Best effort — a
    // device after M024 needs only the key itself.
    auto session_key = SessionKeyWith(peer_identity);
    auto wrapped = session_key
                       ? ICallMediaKeyStore::WrapKeyB64(*session_key, key_bytes, call_id, media_epoch, media_key_id)
                       : Roe<std::string>(session_key.error());
    if (wrapped) {
      key_detail.wrapped_key_b64 = *wrapped;
    } else {
      log().info << "media key's old form not wrapped call_id=" << call_id << " peer=" << peer_identity << ": "
                 << wrapped.error().message;
    }
  }
  auto key_json = CallControlCodec::EncodeMediaKey(key_detail);
  if (!key_json) {
    return key_json.error();
  }
  return SendDirect(peer_identity, CallControlType::CallMediaKey, *key_json, "Call media key");
}

} // namespace pbr
