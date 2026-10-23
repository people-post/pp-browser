#include "domain/delivery/PeerRouteBook.h"

#include "domain/peers/DirectoryShadowCache.h"
#include "lib/messaging/RelayReceivePipeline.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/PeerBriefRoute.h"

namespace pbr {

PeerRouteBook::PeerRouteBook(ContactsStore& contacts) : contacts_(contacts) {
  redirectLogger("PeerRouteBook");
}

void PeerRouteBook::SetSources(DirectoryShadowCache* shadows, mesh::IDirectoryClient* directory) {
  shadows_ = shadows;
  directory_ = directory;
}

void PeerRouteBook::NoteAccountRoute(const std::string& account_id, const std::string& relay_user_id) {
  if (!IsAccountIdentityValue(account_id) || !IsRelayUserIdValue(relay_user_id)) {
    return;
  }
  // A route only — never a directory answer: the directory cache holds what the directory said.
  std::lock_guard lock(mutex_);
  account_to_relay_[account_id] = relay_user_id;
}

void PeerRouteBook::RememberFromEnvelope(const RelayEnvelope& envelope, const RelayReceiveOutcome& outcome) {
  // Learned only from an envelope whose signature verified: its sender_relay_id is then the
  // sender's own signed claim, not whatever an unauthenticated envelope said.
  if (!outcome.sender_verified) {
    return;
  }
  NoteAccountRoute(envelope.sender_contact_id, envelope.sender_relay_id);
}

std::optional<std::string> PeerRouteBook::Resolve(const Thread& thread) const {
  std::unordered_map<std::string, std::string> learned;
  {
    std::lock_guard lock(mutex_);
    learned = account_to_relay_;
  }
  if (auto from_local = ResolvePeerBriefRoute(thread, contacts_, learned)) {
    return from_local;
  }
  if (shadows_ && IsAccountIdentityValue(thread.peer_identity_value)) {
    if (auto hit = shadows_->Get(thread.peer_identity_value)) {
      if (auto relay = RelayUserIdFromDirectoryHit(*hit)) {
        return relay;
      }
    }
  }
  return std::nullopt;
}

namespace {

Thread AsPeer(const std::string& account_id) {
  Thread thread;
  thread.kind = ThreadKind::Direct;
  thread.peer_identity_value = account_id;
  return thread;
}

} // namespace

std::optional<std::string> PeerRouteBook::ResolveAccount(const std::string& account_id) const {
  return Resolve(AsPeer(account_id));
}

std::optional<std::string> PeerRouteBook::ResolveAccountWithDirectory(const std::string& account_id) {
  return ResolveWithDirectory(AsPeer(account_id));
}

std::optional<std::string> PeerRouteBook::ResolveWithDirectory(const Thread& thread) {
  if (auto resolved = Resolve(thread)) {
    return resolved;
  }
  if (!directory_ || !IsAccountIdentityValue(thread.peer_identity_value)) {
    return std::nullopt;
  }
  auto hit = directory_->LookupByAccount(thread.peer_identity_value);
  if (!hit) {
    log().warning << "LookupByAccount for Brief route failed peer=" << thread.peer_identity_value
                  << " err=" << hit.error().message;
    return std::nullopt;
  }
  auto relay = RelayUserIdFromDirectoryHit(*hit);
  if (!relay) {
    return std::nullopt;
  }
  NoteAccountRoute(thread.peer_identity_value, *relay);
  if (shadows_) {
    shadows_->Put(*hit);  // the directory's own answer
  }
  return relay;
}

} // namespace pbr
