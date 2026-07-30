#pragma once

#include "base/data/Config.h"
#include "common/Error.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

inline constexpr const char* kMediaRelayProtocolId = "/pp-browser/media-relay/1.0.0";

/** N021 channel_type QoS classes (not codecs). */
enum class MediaChannelType : uint8_t {
  ReliableOrdered = 0,
  LatestLossy = 1,
  BestEffort = 2,
};

struct MediaRelayQuoteRequest {
  std::string call_id;
  int participants = 1;
  int64_t want_up_bps = 0;
  int64_t want_down_bps = 0;
};

struct MediaRelayQuote {
  bool ok = false;
  std::string error;
  std::string quote_id;
  int64_t a_up_bps = 0;
  int64_t a_down_bps = 0;
  int64_t b_up_bps = 0;
  int64_t b_down_bps = 0;
  std::string pricing_mode = "volunteer";
  double rate = 0.0;
  int64_t ceiling_bytes = 0;
  double ceiling_amount = 0.0;
};

struct MediaRelayAttachResult {
  bool ok = false;
  std::string error;
  std::string session_token;
};

struct MediaRelayAdmissionPolicy {
  bool prefer_contacts_only = false;
  std::unordered_set<std::string> contact_peer_ids;
};

struct MediaDataFrame {
  uint32_t stream_id = 0;
  uint16_t channel_id = 0;
  MediaChannelType channel_type = MediaChannelType::BestEffort;
  uint32_t seq = 0;
  uint8_t mark = 0;
  std::vector<uint8_t> payload;
};

/** Encode/decode N021 data frames (body after u64-BE length prefix). */
std::vector<uint8_t> EncodeMediaDataFrame(const MediaDataFrame& frame);
Roe<MediaDataFrame> DecodeMediaDataFrame(const std::vector<uint8_t>& body);

/**
 * Blind multiplexed media forwarder (n4-media / N018–N021).
 * Control: length-prefixed JSON. Data: length-prefixed binary frames (version byte 1).
 */
class MediaRelayService {
public:
  MediaRelayService(Libp2pHost& host, PeerSessionManager& sessions);
  ~MediaRelayService();

  MediaRelayService(const MediaRelayService&) = delete;
  MediaRelayService& operator=(const MediaRelayService&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  void SetBudget(const MediaRelayBudgetConfig& budget);
  void SetPricing(const RelayPricingConfig& pricing);
  void SetAdmissionPolicy(MediaRelayAdmissionPolicy policy);

  /** Client: request ↑/↓ quote from hop. */
  Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key, const MediaRelayQuoteRequest& request,
                                    int timeout_ms = 8000);

  /**
   * Client: accept quote + attach session. Keeps control stream open for data/subscribe.
   * `on_frame` receives fan-in frames from the hop.
   */
  Roe<MediaRelayAttachResult> AcceptAndAttach(
      const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
      const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame, int timeout_ms = 8000);

  Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id);
  Roe<void> Unsubscribe(uint32_t stream_id, uint16_t channel_id);
  Roe<void> SendFrame(const MediaDataFrame& frame);
  void Detach();

  bool IsAttached() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
