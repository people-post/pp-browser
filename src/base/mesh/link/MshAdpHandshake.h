#pragma once

#include "base/mesh/session/MshHandshake.h"
#include "base/mesh/session/Types.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace pbr::amp {

struct MshAdpEstablished {
  SessionMaterial local_material;
  ByteVector master_ikm;
  ByteVector transcript_hash;
};

/** One-sided MSH v1 driver over ADP Reliable payloads. */
class MshAdpHandshake {
public:
  enum class Role { Initiator, Responder };

  using SendWire = std::function<Roe<void>(std::vector<uint8_t> adp_payload)>;
  using CompleteHandler = std::function<void(Roe<MshAdpEstablished>)>;

  MshAdpHandshake(Role role, MshIdentity identity, SendWire send, CompleteHandler on_complete);

  /** Begin handshake (initiator only). */
  Roe<void> Start();

  /** Feed decoded MSH message body (without carrier header). */
  Roe<void> HandleMsh(MshMessageType type, std::span<const uint8_t> body);

  bool IsComplete() const { return complete_; }

private:
  struct KemState {
    ByteVector ephemeral_secret;
    ByteVector ephemeral_public;
  };

  Roe<KemState> NewEphemeralKem();
  Roe<std::vector<uint8_t>> EncodeHello(MshMessageType type, const KemState& kem);
  Roe<MshPayload> BuildLocalPayload(const KemState& kem, const ByteVector& remote_kem_pk, ByteVector& shared_secret_out);
  Roe<void> SendMessage(MshMessageType type, std::span<const uint8_t> body);
  Roe<void> Fail(std::string message);
  Roe<void> MaybeComplete();

  Role role_;
  MshIdentity identity_;
  SendWire send_;
  CompleteHandler on_complete_;
  bool complete_ = false;
  bool started_ = false;

  std::optional<KemState> local_kem_;
  std::optional<MshHello> remote_hello_;
  std::optional<MshPayload> remote_payload_;
  std::vector<ByteVector> transcript_;
  ByteVector client_ss_;
  ByteVector master_ikm_;
};

} // namespace pbr::amp
