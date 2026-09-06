#include "domain/messaging/PeerAnnouncePublisher.h"

#include "domain/messaging/PeerAnnounceCodec.h"

#include "common/PbrCompat.h"

namespace pbr {

PeerAnnouncePublisher::PeerAnnouncePublisher(std::string peer_id, std::vector<uint8_t> mldsa_secret_key,
                                             PeerAnnounceFeed* local_feed)
    : peer_id_(std::move(peer_id)), mldsa_secret_key_(std::move(mldsa_secret_key)), local_feed_(local_feed) {}

Roe<PeerAnnounceTip> PeerAnnouncePublisher::Emit(PeerAnnounceTip tip, const int64_t now_ms) {
  tip.schema_version = kPeerAnnounceTipSchemaVersion;
  tip.peer_id = peer_id_;
  tip.created_at_ms = now_ms;
  auto signed_tip = SignPeerAnnounceTip(std::move(tip), mldsa_secret_key_);
  if (!signed_tip) {
    return signed_tip.error();
  }
  if (local_feed_ != nullptr) {
    if (auto ingested = local_feed_->Ingest(*signed_tip); !ingested) {
      return ingested.error();
    }
  }
  // live_chat tips share seq space but must not become the heartbeat baseline.
  if (TipIsProgramKind(*signed_tip)) {
    last_tip_ = *signed_tip;
    last_emit_ms_ = now_ms;
  }
  next_seq_ = signed_tip->seq + 1;
  epoch_ = signed_tip->epoch;
  return *signed_tip;
}

Roe<PeerAnnounceTip> PeerAnnouncePublisher::Publish(const Draft& draft, const int64_t now_ms) {
  if (peer_id_.empty()) {
    return Error("peer announce publisher missing peer_id");
  }
  if (draft.topic_id.empty() || draft.program_id.empty()) {
    return Error("peer announce publish requires topic_id and program_id");
  }

  PeerAnnounceTip tip;
  tip.topic_id = draft.topic_id;
  tip.program_id = draft.program_id;
  tip.state = draft.state;
  tip.join_handle = draft.join_handle;
  tip.hop_peer_id = draft.hop_peer_id;
  tip.l1_hop_peer_ids = draft.l1_hop_peer_ids;
  tip.kind = draft.kind;
  tip.viewer_peer_id = draft.viewer_peer_id;
  tip.viewer_msg_id = draft.viewer_msg_id;
  tip.body = draft.body;
  tip.content_id_hex = draft.content_id_hex;

  if (draft.bump_epoch) {
    tip.epoch = epoch_ + 1;
    tip.seq = 1;
  } else {
    tip.epoch = epoch_;
    tip.seq = next_seq_;
  }
  return Emit(std::move(tip), now_ms);
}

Roe<std::optional<PeerAnnounceTip>> PeerAnnouncePublisher::MaybeEmitHeartbeat(const int64_t now_ms,
                                                                              const double jitter_unit) {
  if (!last_tip_ || last_tip_->state != PeerAnnounceState::Live) {
    return std::optional<PeerAnnounceTip>{};
  }
  if (!PeerAnnounceHeartbeatDue(last_emit_ms_, now_ms)) {
    return std::optional<PeerAnnounceTip>{};
  }
  (void)jitter_unit;

  PeerAnnounceTip tip = *last_tip_;
  tip.seq = next_seq_;
  tip.body.clear();
  tip.content_id_hex.clear();
  tip.kind.clear();
  tip.viewer_peer_id.clear();
  tip.viewer_msg_id.clear();
  tip.state = PeerAnnounceState::Live;
  auto emitted = Emit(std::move(tip), now_ms);
  if (!emitted) {
    return emitted.error();
  }
  return std::optional<PeerAnnounceTip>{*emitted};
}

} // namespace pbr
