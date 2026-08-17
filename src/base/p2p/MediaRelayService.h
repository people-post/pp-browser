#pragma once

#include "base/data/Config.h"
#include "base/media/CallMediaHealth.h"
#include "base/people/RelayScope.h"
#include "common/Error.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/PeerSessionManager.h"
#include "base/p2p/RelayRuntimeStats.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace pbr {

inline constexpr const char* kMediaRelayProtocolId = "/pp-browser/media-relay/1.0.0";

/** Per-inbound-stream control/attach phases (N026 / MEDIA_RELAY_ATTACH.md). */
enum class MediaRelayAttachPhase {
  Control = 0,
  Quoted,
  Accepted,
  Attaching,
  Attached,
  Rejected,
  Closed,
};

enum class MediaRelayAttachEvent {
  StreamOpened = 0,
  OpQuote,
  OpAccept,
  OpAttach,
  OpUnsupported,
  AdmitFail,
  AttachOk,
  AttachFail,
  Cancel,
};

const char* MediaRelayAttachPhaseName(MediaRelayAttachPhase phase);
const char* MediaRelayAttachEventName(MediaRelayAttachEvent ev);

/** Client outbound attach phases (phone→hop AcceptAndAttach) — N026. */
enum class MediaRelayClientPhase {
  Idle = 0,
  Dialing,
  Accepting,
  Attaching,
  Attached,
  Detaching,
};

enum class MediaRelayClientEvent {
  AttachRequested = 0,
  OpenStreamOk,
  OpenStreamFail,
  AcceptOk,
  AcceptFail,
  AttachOk,
  AttachFail,
  DetachRequested,
  AttachTimeout,
  DuplexLost,
  AttachSuperseded,
};

const char* MediaRelayClientPhaseName(MediaRelayClientPhase phase);
const char* MediaRelayClientEventName(MediaRelayClientEvent ev);

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
  RelayScopeMask serve_scope_mask = kRelayScopeVolunteerServe;
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

class MediaRelayRuntime;

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

  /** Active HostSession / participant aggregates for status chrome (S008/S009). */
  MediaRelayRuntimeStats RuntimeStats() const;

  /** Local libp2p PeerId (base58); SoftMigrate PreferLocalMediaHop / AttachAsLocalHop. */
  Roe<std::string> LocalPeerIdBase58() const { return host_.LocalPeerIdBase58(); }

  void SetBudget(const MediaRelayBudgetConfig& budget);
  void SetPricing(const RelayPricingConfig& pricing);
  void SetAdmissionPolicy(MediaRelayAdmissionPolicy policy);

  /** Client: request ↑/↓ quote from hop. */
  Roe<MediaRelayQuote> RequestQuote(const std::string& hop_peer_key, const MediaRelayQuoteRequest& request,
                                    int timeout_ms = 8000);

  /**
   * Client: accept quote + attach session. Keeps control stream open for data/subscribe.
   * Does **not** start the inbound frame reader — call `StartClientFrameReader()` after the
   * app media engine is ready (StartSfu) so SoftMigrate TearDown cannot race decode.
   */
  Roe<MediaRelayAttachResult> AcceptAndAttach(
      const std::string& hop_peer_key, const std::string& quote_id, const std::string& call_id,
      const std::string& auth_stub, std::function<void(MediaDataFrame)> on_frame, int timeout_ms = 8000);

  /** Begin reading fan-in frames after AcceptAndAttach + media StartSfu. Idempotent. */
  void StartClientFrameReader();

  /**
   * Guest duplex died unexpectedly (not Detach). Invoked on the libp2p io thread —
   * callers should bounce to UI/worker before re-AcceptAndAttach.
   */
  void SetClientTransportLostHandler(std::function<void()> handler);

  /**
   * In-call hop: join the local HostSession as a publisher without dialing self.
   * SoftMigrate PreferLocalMediaHop (or PreferInCall when hop=local) uses this path;
   * opening the session also unlocks call-scoped admission for stranger joiners.
   */
  Roe<MediaRelayAttachResult> AttachAsLocalHop(const std::string& call_id,
                                               std::function<void(MediaDataFrame)> on_frame);

  Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id);
  Roe<void> Unsubscribe(uint32_t stream_id, uint16_t channel_id);
  Roe<void> SendFrame(const MediaDataFrame& frame);
  void Detach();

  /**
   * Enqueue a raw client→hop body (length-prefixed by DuplexFrameSession).
   * Loopback goldens only (corrupt-frame skip); not a product API.
   */
  Roe<void> EnqueueRawClientBodyForTest(std::vector<uint8_t> body);

  bool IsAttached() const;
  /** Diagnostics: client outbound attach phase (remote hop path). */
  MediaRelayClientPhase ClientPhase() const;
  /** True while SoftMigrate PreferLocal is publishing into the local HostSession. */
  bool IsLocalHopAttached() const;

  /**
   * Path pressure 0..1 from recent hop drops (V032). Clients may feed CallMediaAdaptation.
   * Meaningful while attached (local hop or remote client).
   */
  double PathPressure() const;
  /** Hop drop counters for chrome / logs (V032). */
  CallHopHealth HealthSnapshot() const;

  /** V032 host load limits (also documented in HOST_RECEIVE_POLICY). */
  static constexpr size_t kMaxHostSessions = 4;
  static constexpr size_t kMaxParticipantsPerSession = 8;

private:
  std::shared_ptr<MediaRelayRuntime> runtime_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
