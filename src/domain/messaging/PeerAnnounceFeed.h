#pragma once

#include "domain/messaging/PeerAnnounceTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/** In-memory tip feed: verify, seq/epoch dedup. No epidemic relay (Spine B). */
class PeerAnnounceFeed {
public:
  explicit PeerAnnounceFeed(std::vector<uint8_t> publisher_public_key = {});

  void SetTrustedPublisherKey(std::vector<uint8_t> publisher_public_key);

  Roe<void> Ingest(const PeerAnnounceTip& tip);

  std::optional<PeerAnnounceTip> Latest(const std::string& peer_id, const std::string& topic_id,
                                        const std::string& program_id) const;

  std::vector<PeerAnnounceTip> ListForTopic(const std::string& peer_id, const std::string& topic_id) const;

  bool IsNewerThanStored(const PeerAnnounceTip& tip) const;

  size_t Size() const { return tips_.size(); }

private:
  static std::string MapKey(const std::string& peer_id, const std::string& topic_id,
                            const std::string& program_id);

  std::vector<uint8_t> publisher_public_key_;
  std::unordered_map<std::string, PeerAnnounceTip> tips_;
};

} // namespace pbr
