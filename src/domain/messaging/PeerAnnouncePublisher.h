#pragma once

#include "domain/messaging/PeerAnnounceFeed.h"
#include "domain/messaging/PeerAnnounceTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * Local publisher for Spine B: builds signed tips, tracks seq/epoch, optional local feed write.
 * Heartbeats only while state=live and the min interval has elapsed. No Amp / mesh fan-out.
 */
class PeerAnnouncePublisher {
public:
  struct Draft {
    std::string topic_id;
    std::string program_id;
    PeerAnnounceState state = PeerAnnounceState::Scheduled;
    std::string join_handle;
    std::string body;
    std::string content_id_hex;
    /** When true, bump epoch and reset seq to 1 (new show / revoke path). */
    bool bump_epoch = false;
  };

  PeerAnnouncePublisher(std::string peer_id, std::vector<uint8_t> mldsa_secret_key,
                        PeerAnnounceFeed* local_feed = nullptr);

  const std::string& PeerId() const { return peer_id_; }
  uint64_t Epoch() const { return epoch_; }
  uint64_t NextSeq() const { return next_seq_; }
  int64_t LastEmitMs() const { return last_emit_ms_; }
  std::optional<PeerAnnounceTip> LastTip() const { return last_tip_; }

  /** Event tip (go-live / end / schedule). Bypasses heartbeat floor. */
  Roe<PeerAnnounceTip> Publish(const Draft& draft, int64_t now_ms);

  /**
   * If last tip is live and heartbeat is due, emit a compact re-announce (clears body).
   * Returns nullopt when not due / not live.
   */
  Roe<std::optional<PeerAnnounceTip>> MaybeEmitHeartbeat(int64_t now_ms, double jitter_unit = 0.0);

private:
  Roe<PeerAnnounceTip> Emit(PeerAnnounceTip tip, int64_t now_ms);

  std::string peer_id_;
  std::vector<uint8_t> mldsa_secret_key_;
  PeerAnnounceFeed* local_feed_ = nullptr;
  uint64_t epoch_ = 0;
  uint64_t next_seq_ = 1;
  int64_t last_emit_ms_ = 0;
  std::optional<PeerAnnounceTip> last_tip_;
};

} // namespace pbr
