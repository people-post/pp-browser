#pragma once

#include "domain/mesh/host/MeshPorts.h"
#include "domain/messaging/BroadcastJoinTicket.h"
#include "domain/messaging/BroadcastLadderLogic.h"
#include "domain/messaging/BroadcastRpcCodec.h"

#include "foundation/crypto/CryptoTypes.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * `/pp-browser/rpc/broadcast/1.0.0` ticket mint + ladder admit/redirect + slot-win
 * over AMP ChannelSession (Spine F). Domain codecs/ladder stay Amp-free.
 */
class AmpBroadcastTransport {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;
  /** Resolve publisher ML-DSA-65 public key (device key) for ticket verify. */
  using ResolvePublisherKey = std::function<std::optional<ByteVector>(const std::string& peer_id)>;
  /** Local publisher secret for minting tickets (device ML-DSA). */
  using ResolvePublisherSecret = std::function<std::optional<ByteVector>()>;
  /** Optional pairwise session key for wrapped_key_b64 mint/verify. */
  using ResolveViewerPairwiseKey = std::function<std::optional<ByteVector>(const std::string& viewer_peer_id)>;
  using ResolveNowMs = std::function<int64_t()>;

  /** Publisher-side media key material for an active live program. */
  struct LiveProgramKey {
    std::string publisher_peer_id;
    ByteVector media_key_bytes;
    uint32_t media_epoch = 1;
    std::string media_key_id;
    std::string hop_peer_id;
    /** Absolute expiry passed into minted tickets (0 → service picks now+ttl). */
    int64_t expires_at_ms = 0;
    int64_t ticket_ttl_ms = 24 * 60 * 60 * 1000;
  };

  /** Local hop capacity for viewer admit-or-redirect (B007). */
  struct HopAttachContext {
    size_t free_viewer_slots = 0;
    std::vector<std::string> whitelist_online_children;
    std::string self_peer_id;
    size_t max_redirect_hints = kDefaultBroadcastMaxRedirectHints;
    double jitter_unit = 0.0;
  };

  /** Local hop capacity for relay slot-win (B007). */
  struct HopSlotWinContext {
    size_t free_child_slots = 0;
    bool candidate_on_whitelist = false;
    bool slot_win_rate_limited = false;
    std::vector<std::string> demotable_viewer_peer_ids;
    size_t max_demotions = 1;
  };

  using ResolveHopAttachContext =
      std::function<HopAttachContext(const std::string& program_id, const std::string& join_handle)>;
  using ResolveHopSlotWinContext = std::function<HopSlotWinContext(
      const std::string& program_id, const std::string& join_handle, const std::string& relay_peer_id)>;

  AmpBroadcastTransport(IChatPeerLinks& links, IoPump io_pump, WorkerPost post_worker = {});
  ~AmpBroadcastTransport();

  AmpBroadcastTransport(const AmpBroadcastTransport&) = delete;
  AmpBroadcastTransport& operator=(const AmpBroadcastTransport&) = delete;

  void Start();
  void Stop();

  void SetPublisherKeyResolver(ResolvePublisherKey resolve_key);
  void SetPublisherSecretResolver(ResolvePublisherSecret resolve_secret);
  void SetViewerPairwiseKeyResolver(ResolveViewerPairwiseKey resolve_pairwise);
  void SetNowMsResolver(ResolveNowMs resolve_now);
  void SetHopAttachResolver(ResolveHopAttachContext resolve_attach);
  void SetHopSlotWinResolver(ResolveHopSlotWinContext resolve_slot_win);

  /** Register / replace media key used when minting tickets for this live program. */
  void PutLiveProgramKey(const std::string& program_id, const std::string& join_handle, LiveProgramKey key);
  void ClearLiveProgramKey(const std::string& program_id, const std::string& join_handle);

  bool IsPeerReachable(const std::string& peer_identity_value) const;

  Roe<BroadcastTicketResponse> RequestTicket(const std::string& peer_key, const BroadcastTicketRequest& req);
  Roe<BroadcastViewerAttachResult> RequestViewerAttach(const std::string& peer_key,
                                                       const BroadcastViewerAttachRequest& req);
  Roe<BroadcastRelaySlotWinResult> RequestRelaySlotWin(const std::string& peer_key,
                                                       const BroadcastRelaySlotWinRequest& req);

private:
  template <typename ResponseT>
  Roe<ResponseT> RoundTrip(const std::string& peer_key, const std::string& request_json, const char* expect_label,
                           std::function<bool(const BroadcastRpcMessage&)> is_response,
                           std::function<ResponseT(BroadcastRpcMessage&&)> take_response);

  struct Impl;
  std::unique_ptr<Impl> impl_;
  IChatPeerLinks& links_;
  IoPump io_pump_;
  WorkerPost post_worker_;
  bool started_ = false;
};

} // namespace pbr
