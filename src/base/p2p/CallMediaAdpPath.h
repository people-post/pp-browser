#pragma once

#include "base/adp/Clock.h"
#include "base/adp/Connection.h"
#include "base/adp/DatagramIo.h"
#include "base/adp/Endpoint.h"
#include "base/adp/Types.h"
#include "base/crypto/CryptoTypes.h"
#include "base/p2p/CallMediaAdpKey.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace pbr {

/** Negotiated ADP endpoints from call-media hello (A010). */
struct CallMediaAdpHelloOffer {
  uint16_t port = 0;
  std::string ipv4; // dotted quad
  adp::AssocId assoc{};
  bool valid() const { return port != 0 && !ipv4.empty(); }
};

/**
 * 1:1 Opus (channel 0) over ADP BestEffort (A008).
 * Ciphertext on ADP matches stream body from EncryptCallMediaFrame.
 */
class CallMediaAdpPath {
public:
  using MediaHandler = std::function<void(uint8_t channel, const std::vector<uint8_t>& payload)>;

  CallMediaAdpPath() = default;
  ~CallMediaAdpPath();

  CallMediaAdpPath(const CallMediaAdpPath&) = delete;
  CallMediaAdpPath& operator=(const CallMediaAdpPath&) = delete;

  /** Bind local UDP (port 0 = ephemeral). Returns local offer fields for hello. */
  Roe<CallMediaAdpHelloOffer> BindLocal(bool offerer_mints_assoc);

  /** After TCP hello: set crypto + peer endpoint; open ADP connection. */
  Roe<void> Activate(const ByteVector& media_key, const std::string& call_id, uint32_t media_epoch,
                     const CallMediaAdpHelloOffer& remote, MediaHandler on_media);

  void Stop();
  bool IsActive() const;
  bool LooksAlive() const;

  /** Local fields to put on hello / hello_ack. */
  CallMediaAdpHelloOffer LocalOffer() const;

  /** Answerer: adopt offerer's assoc before emitting hello_ack. */
  void SetLocalAssoc(const adp::AssocId& id);

  /** Encrypt + BestEffort send (channel 0). Fails closed → caller falls back to TCP. */
  Roe<void> SendOpus(const std::vector<uint8_t>& opus_payload, uint32_t seq, uint8_t mark);

  /** Drain UDP / drive ADP (call from host tick or before/after send). */
  void Pump();

  /** Test injection: use MemoryDatagramIo instead of OsUdp. */
  void SetIoForTest(std::shared_ptr<adp::DatagramIo> io, std::shared_ptr<adp::Clock> clock);

private:
  Roe<void> EnsureEndpoint();

  mutable std::mutex mu_;
  std::shared_ptr<adp::DatagramIo> io_;
  std::shared_ptr<adp::Clock> clock_;
  std::unique_ptr<adp::Endpoint> endpoint_;
  std::shared_ptr<adp::Connection> conn_;
  CallMediaAdpHelloOffer local_{};
  ByteVector media_key_;
  std::string call_id_;
  uint32_t media_epoch_ = 1;
  MediaHandler on_media_;
  bool active_ = false;
  bool test_io_ = false;
};

/** Best-effort primary IPv4 for hello `adp_ip` (may be empty). */
std::string GuessPrimaryIpv4();

bool ParseIpv4Dotted(const std::string& s, uint8_t out[4]);

} // namespace pbr
