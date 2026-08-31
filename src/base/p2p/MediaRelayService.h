#pragma once

#include "base/p2p/MediaRelayTypes.h"
#include "base/p2p/RelayRuntimeStats.h"

#include "common/PbrCompat.h"

#include <functional>
#include <memory>
#include <string>

namespace pbr {

/**
 * Legacy TCP media-relay service (Yamux/Noise). Product path is AmpMediaRelayCoordinator (D10/A017).
 * Header retained only while residual TCP sources/tests are unlinked.
 */
class Libp2pHost;
class PeerSessionManager;
class MediaRelayRuntime;

class MediaRelayService {
public:
  MediaRelayService(Libp2pHost& host, PeerSessionManager& sessions);
  ~MediaRelayService();

  MediaRelayService(const MediaRelayService&) = delete;
  MediaRelayService& operator=(const MediaRelayService&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  MediaRelayRuntimeStats RuntimeStats() const;
  Roe<std::string> LocalPeerIdBase58() const;

  void SetBudget(const MediaRelayBudgetConfig& budget);
  void SetPricing(const RelayPricingConfig& pricing);
  void SetAdmissionPolicy(MediaRelayAdmissionPolicy policy);

  Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key, const MediaRelayQuoteRequest& request,
                                    int timeout_ms = 8000);
  Roe<MediaRelayAttachResult> AcceptAndAttach(
      const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
      const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame, int timeout_ms = 8000);
  void StartClientFrameReader();
  void SetClientTransportLostHandler(std::function<void()> handler);
  Roe<MediaRelayAttachResult> AttachAsLocalHop(const std::string& call_id,
                                               std::function<void(MediaDataFrame)> on_frame);
  Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id);
  Roe<void> Unsubscribe(uint32_t stream_id, uint16_t channel_id);
  Roe<void> SendFrame(const MediaDataFrame& frame);
  void Detach();
  Roe<void> EnqueueRawClientBodyForTest(std::vector<uint8_t> body);
  bool IsAttached() const;
  MediaRelayClientPhase ClientPhase() const;
  bool IsLocalHopAttached() const;
  double PathPressure() const;
  CallHopHealth HealthSnapshot() const;

  static constexpr size_t kMaxHostSessions = kMediaRelayMaxHostSessions;
  static constexpr size_t kMaxParticipantsPerSession = kMediaRelayMaxParticipantsPerSession;

private:
  std::shared_ptr<MediaRelayRuntime> runtime_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
