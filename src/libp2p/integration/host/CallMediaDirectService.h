#pragma once

#include "base/crypto/CryptoTypes.h"
#include "common/Error.h"
#include "libp2p/integration/host/Libp2pHost.h"
#include "libp2p/integration/host/PeerSessionManager.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

inline constexpr const char* kCallMediaDirectProtocolId = "/pp-browser/call-media/1.0.0";

struct CallMediaDirectConnectParams {
  std::string peer_key;
  std::string call_id;
  uint32_t media_epoch = 1;
  ByteVector media_key;
  bool offerer = true;
};

struct CallMediaDirectCallbacks {
  std::function<void()> on_connected;
  std::function<void(const std::vector<uint8_t>& opus_payload)> on_audio;
  std::function<void(const std::string& error)> on_failed;
};

/**
 * 1:1 libp2p call-media transport (m1 / V026).
 * Opus payloads are AEAD-encrypted under the shared call media key before send.
 */
class CallMediaDirectService {
public:
  CallMediaDirectService(Libp2pHost& host, PeerSessionManager& sessions);
  ~CallMediaDirectService();

  CallMediaDirectService(const CallMediaDirectService&) = delete;
  CallMediaDirectService& operator=(const CallMediaDirectService&) = delete;

  void Start();
  void Stop();
  bool IsStarted() const { return started_; }

  /** Register handler for inbound streams (answerer side). */
  void SetInboundHandler(
      std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler);

  /** Active outbound/inbound session. */
  bool IsActive() const;
  void Detach();

  /** Client: dial peer and run hello handshake; starts reader thread. */
  Roe<void> Connect(const CallMediaDirectConnectParams& params, CallMediaDirectCallbacks callbacks,
                    int timeout_ms = 15000);

  /** Send encrypted Opus frame on active stream. */
  Roe<void> SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark = 0);

private:
  struct Impl;
  std::shared_ptr<Impl> impl_;
  Libp2pHost& host_;
  PeerSessionManager& sessions_;
  bool started_ = false;
};

} // namespace pbr
