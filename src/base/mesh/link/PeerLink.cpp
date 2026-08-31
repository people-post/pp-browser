#include "base/mesh/link/PeerLink.h"

#include "base/mesh/link/AmpAdpCarrier.h"
#include "base/mesh/link/PeerLinkManager.h"

namespace pbr::amp {

PeerLink::PeerLink(std::string peer_key, std::string remote_peer_id, const bool outbound,
                   std::shared_ptr<adp::Connection> connection, MshIdentity local_identity, PeerLinkManager& owner)
    : peer_key_(std::move(peer_key)), remote_peer_id_(std::move(remote_peer_id)), outbound_(outbound),
      connection_(std::move(connection)), identity_(std::move(local_identity)), owner_(owner) {
  connection_->OnMessage([this](const adp::Message& message) {
    HandleAdpPayload(message.payload);
  });
}

void PeerLink::StartOutboundHandshake(CompleteCb on_established) {
  establish_cb_ = std::move(on_established);
  phase_ = PeerLinkPhase::Handshaking;
  const auto role = MshAdpHandshake::Role::Initiator;
  handshake_ = std::make_unique<MshAdpHandshake>(
      role, identity_,
      [this](std::vector<uint8_t> payload) { return SendAdp(std::move(payload), adp::QosClass::Reliable); },
      [this](Roe<MshAdpEstablished> established) { OnHandshakeComplete(std::move(established)); });
  if (auto started = handshake_->Start(); !started) {
    FailAssociation(started.error());
  }
}

void PeerLink::StartInboundHandshake(CompleteCb on_established) {
  establish_cb_ = std::move(on_established);
  phase_ = PeerLinkPhase::Handshaking;
  const auto role = MshAdpHandshake::Role::Responder;
  handshake_ = std::make_unique<MshAdpHandshake>(
      role, identity_,
      [this](std::vector<uint8_t> payload) { return SendAdp(std::move(payload), adp::QosClass::Reliable); },
      [this](Roe<MshAdpEstablished> established) { OnHandshakeComplete(std::move(established)); });
}

void PeerLink::HandleAdpPayload(const std::span<const uint8_t> payload) {
  if (phase_ == PeerLinkPhase::Connected && mux_) {
    auto kind = AmpAdpCarrier::DecodeKind(payload);
    if (!kind) {
      return;
    }
    if (*kind == AmpAdpPayloadKind::Sealed) {
      auto header = AmpAdpCarrier::DecodeSealedHeader(payload);
      if (!header) {
        return;
      }
      auto body = AmpAdpCarrier::DecodeSealedBody(payload);
      if (!body) {
        return;
      }
      (void)mux_->OnSealedInbound(header->first, header->second, *body);
    }
    return;
  }

  if (!handshake_) {
    return;
  }
  auto kind = AmpAdpCarrier::DecodeKind(payload);
  if (!kind) {
    return;
  }
  if (*kind == AmpAdpPayloadKind::MshChunk) {
    auto chunk = AmpAdpCarrier::DecodeMshChunk(payload);
    if (!chunk) {
      return;
    }
    auto assembled = PushMshChunk(std::get<0>(*chunk), std::get<1>(*chunk), std::get<2>(*chunk), std::get<3>(*chunk));
    if (!assembled) {
      FailAssociation(assembled.error());
      return;
    }
    if (!assembled->has_value()) {
      return;
    }
    auto msh_type = AmpAdpCarrier::DecodeMshType(assembled->value());
    if (!msh_type) {
      return;
    }
    auto body = AmpAdpCarrier::DecodeMshBody(assembled->value());
    if (!body) {
      return;
    }
    (void)handshake_->HandleMsh(*msh_type, *body);
    return;
  }
  if (*kind != AmpAdpPayloadKind::Msh) {
    return;
  }
  auto msh_type = AmpAdpCarrier::DecodeMshType(payload);
  if (!msh_type) {
    return;
  }
  auto body = AmpAdpCarrier::DecodeMshBody(payload);
  if (!body) {
    return;
  }
  (void)handshake_->HandleMsh(*msh_type, *body);
}

Roe<std::optional<std::vector<uint8_t>>> PeerLink::PushMshChunk(const MshMessageType type, const uint16_t index,
                                                                 const uint16_t count,
                                                                 const std::span<const uint8_t> chunk) {
  if (count == 0 || index >= count) {
    return Error("amp link: bad msh chunk meta");
  }
  if (msh_chunk_count_ == 0) {
    msh_chunk_type_ = type;
    msh_chunk_count_ = count;
    msh_chunk_parts_.assign(count, {});
  }
  if (type != msh_chunk_type_ || count != msh_chunk_count_) {
    return Error("amp link: msh chunk stream mismatch");
  }
  if (msh_chunk_parts_[index].empty()) {
    msh_chunk_parts_[index].assign(chunk.begin(), chunk.end());
  }
  for (const auto& part : msh_chunk_parts_) {
    if (part.empty()) {
      return std::optional<std::vector<uint8_t>>{};
    }
  }
  std::vector<uint8_t> body;
  for (const auto& part : msh_chunk_parts_) {
    body.insert(body.end(), part.begin(), part.end());
  }
  msh_chunk_count_ = 0;
  msh_chunk_parts_.clear();
  auto wire = AmpAdpCarrier::EncodeMsh(msh_chunk_type_, body);
  if (!wire) {
    return wire.error();
  }
  return std::optional<std::vector<uint8_t>>{std::move(*wire)};
}

Roe<void> PeerLink::SendAdp(std::vector<uint8_t> payload, const adp::QosClass qos) {
  if (!connection_) {
    return Error("amp link: no connection");
  }
  return connection_->Send(qos, payload);
}

void PeerLink::OnHandshakeComplete(Roe<MshAdpEstablished> established) {
  if (!established) {
    FailAssociation(established.error());
    return;
  }
  FinishEstablishment(std::move(*established));
}

void PeerLink::FinishEstablishment(MshAdpEstablished established) {
  master_ikm_ = std::move(established.master_ikm);
  transcript_hash_ = std::move(established.transcript_hash);

  auto session = Session::FromMaterial(established.local_material, master_ikm_, transcript_hash_);
  if (!session) {
    FailAssociation(session.error());
    return;
  }

  connection_->UpgradeBinder(session->AssocKey());
  session_ = std::make_unique<Session>(std::move(*session));
  mux_ = std::make_unique<ChannelMux>(*session_);
  AttachMuxTransport();
  handshake_.reset();
  phase_ = PeerLinkPhase::Connected;
  owner_.OnLinkEstablished(*this);

  if (establish_cb_) {
    establish_cb_(Roe<void>());
    establish_cb_ = nullptr;
  }
}

void PeerLink::FailAssociation(const Error& error) {
  phase_ = PeerLinkPhase::Backoff;
  handshake_.reset();
  if (establish_cb_) {
    establish_cb_(error);
    establish_cb_ = nullptr;
  }
}

void PeerLink::AttachMuxTransport() {
  mux_->SetTransport([this](const uint32_t channel_id, const uint32_t channel_seq, const adp::QosClass qos,
                            std::vector<uint8_t> sealed) {
    auto wire = AmpAdpCarrier::EncodeSealed(channel_id, channel_seq, sealed);
    if (!wire) {
      return;
    }
    (void)SendAdp(std::move(*wire), qos);
  });
}

void PeerLink::MarkWarm() { warm_ = true; }

void PeerLink::ClearWarm() { warm_ = false; }

} // namespace pbr::amp
