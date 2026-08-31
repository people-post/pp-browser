#pragma once

#include "base/adp/Connection.h"
#include "base/mesh/channel/Capability.h"
#include "base/mesh/channel/ChannelMux.h"
#include "base/mesh/link/MshAdpHandshake.h"
#include "base/mesh/link/Types.h"
#include "base/mesh/session/Session.h"

#include "common/Error.h"
#include "common/PbrCompat.h"

#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pbr::amp {

class PeerLinkManager;

/** One ADP association + AMP session + channel mux to a remote peer. Io-thread affine. */
class PeerLink {
public:
  using CompleteCb = std::function<void(Roe<void>)>;

  PeerLink(std::string peer_key, std::string remote_peer_id, const bool outbound,
           std::shared_ptr<adp::Connection> connection, MshIdentity local_identity, PeerLinkManager& owner);

  void StartOutboundHandshake(CompleteCb on_established);
  void StartInboundHandshake(CompleteCb on_established);

  void HandleAdpPayload(std::span<const uint8_t> payload);

  PeerLinkPhase Phase() const { return phase_; }
  bool IsOutbound() const { return outbound_; }
  const std::string& PeerKey() const { return peer_key_; }
  const std::string& RemotePeerId() const { return remote_peer_id_; }
  const ByteVector& RemoteIdentityPublicKey() const { return remote_identity_public_key_; }
  adp::Connection& Connection() { return *connection_; }
  ChannelMux* Mux() { return mux_.get(); }
  Session* GetSession() { return session_.get(); }

  const CapabilityPayload* RemoteCapability() const {
    return remote_capability_ ? &*remote_capability_ : nullptr;
  }

  void MarkWarm();
  void ClearWarm();
  bool IsWarm() const { return warm_; }

private:
  friend class PeerLinkManager;

  void SetPeerKey(std::string peer_key) { peer_key_ = std::move(peer_key); }
  void SetRemoteCapability(CapabilityPayload payload) { remote_capability_ = std::move(payload); }
  bool CapabilityExchangeStarted() const { return capability_exchange_started_; }
  void MarkCapabilityExchangeStarted() { capability_exchange_started_ = true; }
  bool CapabilityOfferSent() const { return capability_offer_sent_; }
  void MarkCapabilityOfferSent() { capability_offer_sent_ = true; }

  Roe<void> SendAdp(std::vector<uint8_t> payload, adp::QosClass qos);
  void OnHandshakeComplete(Roe<MshAdpEstablished> established);
  void FinishEstablishment(MshAdpEstablished established);
  void FailAssociation(const Error& error);
  void AttachMuxTransport();

  std::string peer_key_;
  std::string remote_peer_id_;
  ByteVector remote_identity_public_key_;
  bool outbound_;
  std::shared_ptr<adp::Connection> connection_;
  MshIdentity identity_;
  PeerLinkManager& owner_;
  PeerLinkPhase phase_ = PeerLinkPhase::Handshaking;
  bool warm_ = false;
  bool capability_exchange_started_ = false;
  bool capability_offer_sent_ = false;
  std::optional<CapabilityPayload> remote_capability_;

  std::unique_ptr<MshAdpHandshake> handshake_;
  std::unique_ptr<Session> session_;
  std::unique_ptr<ChannelMux> mux_;
  ByteVector master_ikm_;
  ByteVector transcript_hash_;
  CompleteCb establish_cb_;

  MshMessageType msh_chunk_type_{};
  uint16_t msh_chunk_count_ = 0;
  std::vector<std::vector<uint8_t>> msh_chunk_parts_;
  Roe<std::optional<std::vector<uint8_t>>> PushMshChunk(MshMessageType type, uint16_t index, uint16_t count,
                                                         std::span<const uint8_t> chunk);
};

} // namespace pbr::amp
