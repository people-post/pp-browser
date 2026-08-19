#include "base/p2p/CallMediaDirectService.h"

#include "base/p2p/CallMediaSession.h"

#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

namespace pbr {

namespace {

using libp2p::peer::ProtocolName;

} // namespace

const char* CallMediaSessionPhaseName(const CallMediaSessionPhase phase) {
  switch (phase) {
  case CallMediaSessionPhase::Idle:
    return "Idle";
  case CallMediaSessionPhase::Dialing:
    return "Dialing";
  case CallMediaSessionPhase::HelloOutbound:
    return "HelloOutbound";
  case CallMediaSessionPhase::HelloInbound:
    return "HelloInbound";
  case CallMediaSessionPhase::Adopting:
    return "Adopting";
  case CallMediaSessionPhase::MediaReady:
    return "MediaReady";
  case CallMediaSessionPhase::Detaching:
    return "Detaching";
  }
  return "?";
}

const char* CallMediaSessionEventName(const CallMediaSessionEvent ev) {
  switch (ev) {
  case CallMediaSessionEvent::ConnectRequested:
    return "ConnectRequested";
  case CallMediaSessionEvent::OpenStreamOk:
    return "OpenStreamOk";
  case CallMediaSessionEvent::OpenStreamFail:
    return "OpenStreamFail";
  case CallMediaSessionEvent::InboundStream:
    return "InboundStream";
  case CallMediaSessionEvent::HelloOk:
    return "HelloOk";
  case CallMediaSessionEvent::HelloFail:
    return "HelloFail";
  case CallMediaSessionEvent::AdoptWon:
    return "AdoptWon";
  case CallMediaSessionEvent::AdoptLost:
    return "AdoptLost";
  case CallMediaSessionEvent::DuplexStarted:
    return "DuplexStarted";
  case CallMediaSessionEvent::DuplexEof:
    return "DuplexEof";
  case CallMediaSessionEvent::DuplexError:
    return "DuplexError";
  case CallMediaSessionEvent::DetachRequested:
    return "DetachRequested";
  case CallMediaSessionEvent::ConnectTimeout:
    return "ConnectTimeout";
  case CallMediaSessionEvent::HandlerCleared:
    return "HandlerCleared";
  case CallMediaSessionEvent::ConnectSuperseded:
    return "ConnectSuperseded";
  }
  return "?";
}

CallMediaDirectService::CallMediaDirectService(Libp2pHost& host, PeerSessionManager& sessions)
    : session_(std::make_shared<CallMediaSession>()), host_(host), sessions_(sessions) {
  session_->SetHost(&host_);
}

CallMediaDirectService::~CallMediaDirectService() {
  Stop();
}

void CallMediaDirectService::Start() {
  if (started_ || !host_.IsRunning()) {
    return;
  }
  started_ = true;
  auto session = session_;
  host_.GetHost().setProtocolHandler({ProtocolName{kCallMediaDirectProtocolId}},
                                     [session](libp2p::StreamAndProtocol stream) {
                                       session->HandleInbound(std::move(stream));
                                     });
}

void CallMediaDirectService::Stop() {
  started_ = false;
  ClearInboundHandler();
  Detach();
}

void CallMediaDirectService::SetInboundHandler(
    std::function<void(CallMediaDirectConnectParams&, CallMediaDirectCallbacks&)> handler) {
  session_->SetInboundHandler(std::move(handler));
}

void CallMediaDirectService::ClearInboundHandler() {
  session_->ClearInboundHandler();
}

bool CallMediaDirectService::IsActive() const {
  return session_->IsActive();
}

CallMediaSessionPhase CallMediaDirectService::Phase() const {
  return session_->Phase();
}

void CallMediaDirectService::Detach() {
  session_->Detach();
}

Roe<void> CallMediaDirectService::Connect(const CallMediaDirectConnectParams& params,
                                          CallMediaDirectCallbacks callbacks, int timeout_ms) {
  return session_->Connect(sessions_, params, std::move(callbacks), timeout_ms);
}

Roe<void> CallMediaDirectService::SendAudio(const std::vector<uint8_t>& opus_payload, uint32_t seq,
                                            uint8_t mark) {
  return session_->SendAudio(opus_payload, seq, mark);
}

Roe<void> CallMediaDirectService::SendMedia(uint8_t channel, const std::vector<uint8_t>& payload, uint32_t seq,
                                            uint8_t mark) {
  return session_->SendMedia(channel, payload, seq, mark);
}

} // namespace pbr
