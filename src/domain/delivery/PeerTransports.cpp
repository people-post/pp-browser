#include "domain/delivery/PeerTransports.h"

#include "domain/attachments/AmpChatBlobTransport.h"
#include "domain/delivery/AmpChatHistoryTransport.h"
#include "domain/delivery/AmpDirectChatTransport.h"
#include "foundation/runtime/AppRuntime.h"
#include "lib/people/DirectChatTargetFromContact.h"
#include "lib/people/MeshHopPolicy.h"
#include "mesh/reachability/AmpObservedAddrs.h"
#include "mesh/reachability/Reachability.h"

namespace pbr {

PeerTransports::PeerTransports(IThreadStore& store, IdentityStore& identity,
                               IPeerSigningKeyResolver& signing_key_resolver, IPskSessionStore& psk_store)
    : store_(store), identity_(identity), signing_key_resolver_(signing_key_resolver), psk_store_(psk_store) {
  redirectLogger("PeerTransports");
}

PeerTransports::~PeerTransports() {
  Detach();
}

void PeerTransports::SetProfileDataDir(std::string profile_data_dir) {
  profile_data_dir_ = std::move(profile_data_dir);
  if (blob_) {
    blob_->SetProfileDataDir(profile_data_dir_);
  }
}

IChatBlobPeerClient* PeerTransports::BlobClient() const {
  return blob_.get();
}

IChatBlobPeerService* PeerTransports::BlobService() const {
  return blob_.get();
}

bool PeerTransports::Attach(mesh::IChatPeerLinks* links, std::function<void()> io_pump, WorkerPost worker_post,
                            WorkerPost post_io, PostAfter post_after) {
  if (!links) {
    log().warning << "Attach: no peer links";
    return false;
  }
  if (links_ == links && direct_chat_ && history_ && blob_) {
    return true;  // already attached to these links
  }
  Detach();
  links_ = links;
  if (address_disclosure_) {
    people_links_ = std::make_unique<mesh::DisclosureGatedPeerLinks>(*links_, *address_disclosure_);
  }
  mesh::IChatPeerLinks& people = people_links_ ? static_cast<mesh::IChatPeerLinks&>(*people_links_) : *links_;
  if (!worker_post) {
    worker_post = [](std::function<void()> task) { AppRuntime::PostWorkerNormal(std::move(task)); };
  }

  auto blob = std::make_unique<AmpChatBlobTransport>(people, io_pump, store_, identity_, signing_key_resolver_,
                                                     worker_post, post_io, post_after);
  blob->Start();
  if (!profile_data_dir_.empty()) {
    blob->SetProfileDataDir(profile_data_dir_);
  }
  blob_ = std::move(blob);

  auto history = std::make_unique<AmpChatHistoryTransport>(people, io_pump, store_, identity_, psk_store_,
                                                           signing_key_resolver_, worker_post, post_io, post_after);
  history->Start();
  history_ = std::move(history);

  auto chat = std::make_unique<AmpDirectChatTransport>(people, io_pump, worker_post, post_io, post_after);
  chat->SetInboundHandler([this](RelayEnvelope envelope) {
    if (inbound_) {
      inbound_(std::move(envelope));
    }
  });
  chat->Start();
  direct_chat_ = std::move(chat);
  log().info << "direct chat/history/blob transport=amp";
  return true;
}

void PeerTransports::Detach() {
  // Destructors call Stop() → RemoveProtocolHandler on the links; Amp must still be alive.
  direct_chat_.reset();
  history_.reset();
  blob_.reset();
  people_links_.reset();  // after the transports that hold it
  links_ = nullptr;
}

void PeerTransports::RegisterEndpoint(const std::string& peer_key, const std::string& multiaddr) {
  const bool is_adp = mesh::IsAdpMultiaddr(multiaddr);
  // B6: never register undialable ADP hosts (0.0.0.0 / :: / link-local) into the dial book.
  if (is_adp && !mesh::IsUsableAdpListen(multiaddr)) {
    return;
  }
  if (links_ && is_adp) {
    (void)links_->RegisterEndpoint(peer_key, multiaddr);
  } else if (links_) {
    if (auto* amp_history = dynamic_cast<AmpChatHistoryTransport*>(history_.get())) {
      amp_history->RegisterPeerEndpoint(peer_key, multiaddr);
    }
  }
}

void PeerTransports::RegisterEndpoints(const std::string& peer_key, const std::vector<std::string>& multiaddrs) {
  if (peer_key.empty() || multiaddrs.empty()) {
    return;
  }
  std::vector<std::string> usable;
  usable.reserve(multiaddrs.size());
  bool any_non_adp = false;
  for (const std::string& ma : multiaddrs) {
    if (ma.empty()) {
      continue;
    }
    if (mesh::IsAdpMultiaddr(ma)) {
      if (!mesh::IsUsableAdpListen(ma)) {
        continue;
      }
      usable.push_back(ma);
    } else {
      any_non_adp = true;
      usable.push_back(ma);
    }
  }
  if (usable.empty()) {
    return;
  }
  if (links_ && !any_non_adp) {
    // B28: atomic best-first candidate list (callers pass RankAmpDialMultiaddrs order).
    (void)links_->RegisterEndpoints(peer_key, usable);
    return;
  }
  // Non-ADP / history path: last RegisterEndpoint wins Preferred — register worst→best.
  for (auto it = usable.rbegin(); it != usable.rend(); ++it) {
    RegisterEndpoint(peer_key, *it);
  }
}

void PeerTransports::RegisterContactEndpoints(const Contact& contact) {
  const DirectChatTarget target = DirectChatTargetFromContact(contact, ThreadChannel::E2ePublic);
  if (target.peer_identity_value.empty()) {
    return;
  }
  const mesh::AmpDialLocalContext local_ctx = mesh::CollectAmpDialLocalContext();
  if (!contact.remote.endpoints.empty()) {
    for (const mesh::DirectoryEndpoint& endpoint : contact.remote.endpoints) {
      const auto ranked = mesh::RankAmpDialMultiaddrs(endpoint.multiaddrs, local_ctx);
      RegisterEndpoints(endpoint.peer_id, ranked);
      RegisterEndpoints(target.peer_identity_value, ranked);
    }
  } else if (!contact.multiaddrs.empty()) {
    RegisterEndpoints(target.peer_identity_value, mesh::RankAmpDialMultiaddrs(contact.multiaddrs, local_ctx));
  }
  for (const std::string& peer_id : PeerIdsFromContact(contact)) {
    if (links_) {
      if (auto ma = links_->PreferredMultiaddr(peer_id)) {
        RegisterEndpoint(peer_id, *ma);
        if (target.peer_identity_value != peer_id) {
          RegisterEndpoint(target.peer_identity_value, *ma);
        }
      }
    }
  }
}

void PeerTransports::WarmByKey(const std::string& peer_key) {
  if (!links_ || peer_key.empty()) {
    return;
  }
  links_->MarkWarm(peer_key);
  if (links_->GetLinkSnapshot(peer_key).has_endpoint) {
    links_->EnsureAssociation(peer_key, [](mesh::IChatPeerLinks::LinkRoe) {});
  }
}

} // namespace pbr
