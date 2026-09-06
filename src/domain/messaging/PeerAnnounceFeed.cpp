#include "domain/messaging/PeerAnnounceFeed.h"

#include "domain/messaging/PeerAnnounceCodec.h"

#include "common/PbrCompat.h"

namespace pbr {

PeerAnnounceFeed::PeerAnnounceFeed(std::vector<uint8_t> publisher_public_key)
    : publisher_public_key_(std::move(publisher_public_key)) {}

void PeerAnnounceFeed::SetTrustedPublisherKey(std::vector<uint8_t> publisher_public_key) {
  publisher_public_key_ = std::move(publisher_public_key);
}

std::string PeerAnnounceFeed::ProgramMapKey(const std::string& peer_id, const std::string& topic_id,
                                            const std::string& program_id) {
  return peer_id + '\x1f' + topic_id + '\x1f' + program_id;
}

std::string PeerAnnounceFeed::MapKey(const PeerAnnounceTip& tip) {
  if (TipIsLiveChatKind(tip)) {
    return ProgramMapKey(tip.peer_id, tip.topic_id, tip.program_id) + '\x1f' +
           kPeerAnnounceKindLiveChat + '\x1f' + tip.viewer_msg_id;
  }
  return ProgramMapKey(tip.peer_id, tip.topic_id, tip.program_id);
}

bool PeerAnnounceFeed::IsNewerThanStored(const PeerAnnounceTip& tip) const {
  const auto it = tips_.find(MapKey(tip));
  if (it == tips_.end()) {
    return true;
  }
  const PeerAnnounceTip& prev = it->second;
  if (tip.epoch != prev.epoch) {
    return tip.epoch > prev.epoch;
  }
  return tip.seq > prev.seq;
}

Roe<void> PeerAnnounceFeed::Ingest(const PeerAnnounceTip& tip) {
  if (tip.peer_id.empty() || tip.topic_id.empty() || tip.program_id.empty()) {
    return Error("peer announce ingest missing ids");
  }
  if (TipIsLiveChatKind(tip) && tip.viewer_msg_id.empty()) {
    return Error("live_chat tip requires viewer_msg_id");
  }
  if (!publisher_public_key_.empty()) {
    if (auto verified = VerifyPeerAnnounceTip(tip, publisher_public_key_); !verified) {
      return verified.error();
    }
  }
  if (!IsNewerThanStored(tip)) {
    return Error("peer announce tip not newer than stored");
  }
  tips_[MapKey(tip)] = tip;
  return {};
}

std::optional<PeerAnnounceTip> PeerAnnounceFeed::Latest(const std::string& peer_id, const std::string& topic_id,
                                                        const std::string& program_id) const {
  const auto it = tips_.find(ProgramMapKey(peer_id, topic_id, program_id));
  if (it == tips_.end()) {
    return std::nullopt;
  }
  return it->second;
}

std::vector<PeerAnnounceTip> PeerAnnounceFeed::ListForTopic(const std::string& peer_id,
                                                            const std::string& topic_id) const {
  std::vector<PeerAnnounceTip> out;
  const std::string prefix = peer_id + '\x1f' + topic_id + '\x1f';
  for (const auto& [key, tip] : tips_) {
    if (key.rfind(prefix, 0) == 0) {
      out.push_back(tip);
    }
  }
  return out;
}

std::vector<PeerAnnounceTip> PeerAnnounceFeed::ListLiveChat(const std::string& peer_id, const std::string& topic_id,
                                                            const std::string& program_id) const {
  std::vector<PeerAnnounceTip> out;
  const std::string prefix =
      ProgramMapKey(peer_id, topic_id, program_id) + '\x1f' + kPeerAnnounceKindLiveChat + '\x1f';
  for (const auto& [key, tip] : tips_) {
    if (key.rfind(prefix, 0) == 0) {
      out.push_back(tip);
    }
  }
  return out;
}

} // namespace pbr
