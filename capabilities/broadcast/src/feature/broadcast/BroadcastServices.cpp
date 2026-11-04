#include "feature/broadcast/BroadcastServices.h"

#include "common/Logger.h"

#include <chrono>
#include <utility>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

namespace {

logging::Logger& ServicesLog() {
  static logging::Logger logger = logging::getLogger("BroadcastServices");
  return logger;
}

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

} // namespace

BroadcastServices::BroadcastServices(BroadcastServicesDeps deps) {
  const auto& worker = deps.io.post_worker;
  if (auto secret = deps.device_secret ? deps.device_secret() : std::nullopt; secret && !deps.local_peer_id.empty()) {
    publisher_ = std::make_unique<PeerAnnouncePublisher>(deps.local_peer_id, *secret, &feed_);
  } else {
    ServicesLog().warning << "peer-announce publisher skipped (device identity unavailable)";
  }
  announce_ = std::make_unique<AmpPeerAnnounceTransport>(*deps.links, feed_, deps.io.io_pump, worker,
                                                         deps.publisher_key, deps.io.post_io, deps.io.post_after);
  announce_->SetOnTipIngested([this](const PeerAnnounceTip& tip) {
    notifications_.UpsertFromTip(tip, tip.created_at_ms > 0 ? tip.created_at_ms : 0);
  });
  announce_->Start();
  tickets_ = std::make_unique<AmpBroadcastTransport>(*deps.links, deps.io.io_pump, worker, deps.io.post_io,
                                                     deps.io.post_after);
  tickets_->SetPublisherKeyResolver(deps.publisher_key);
  tickets_->SetPublisherSecretResolver(deps.device_secret);
  tickets_->Start();
}

BroadcastServices::~BroadcastServices() {
  // Remove the protocol handlers while the links are still up.
  tickets_.reset();
  announce_.reset();
}

Roe<void> BroadcastServices::RequirePublisher() const {
  if (!publisher_) {
    return Error("peer-announce publisher unavailable (device identity missing)");
  }
  return {};
}

Roe<PeerAnnounceTip> BroadcastServices::PublishTip(const PeerAnnouncePublisher::Draft& draft) {
  if (auto ok = RequirePublisher(); !ok) {
    return ok.error();
  }
  return publisher_->Publish(draft, NowMs());
}

void BroadcastServices::PublishAndPushAsync(const std::string& peer_key, const PeerAnnouncePublisher::Draft& draft,
                                            const int64_t now_ms,
                                            std::function<void(Roe<PeerAnnounceTipAck>)> on_done) {
  if (!on_done) {
    return;
  }
  if (auto ok = RequirePublisher(); !ok) {
    on_done(ok.error());
    return;
  }
  auto tip = publisher_->Publish(draft, now_ms);
  if (!tip) {
    on_done(tip.error());
    return;
  }
  announce_->PushTipAsync(peer_key, *tip, std::move(on_done));
}

Roe<PeerAnnounceTipAck> BroadcastServices::PublishAndPush(const std::string& peer_key,
                                                          const PeerAnnouncePublisher::Draft& draft,
                                                          const int64_t now_ms) {
  if (auto ok = RequirePublisher(); !ok) {
    return ok.error();
  }
  auto tip = publisher_->Publish(draft, now_ms);
  if (!tip) {
    return tip.error();
  }
  return announce_->PushTip(peer_key, *tip);
}

void BroadcastServices::PublishLiveChatFromOverlayAsync(const std::string& peer_key, const std::string& topic_id,
                                                        const std::string& program_id, const std::string& join_handle,
                                                        const std::string& viewer_peer_id,
                                                        const AnnounceOverlayReplyBody& body, const int64_t now_ms,
                                                        std::function<void(Roe<PeerAnnounceTipAck>)> on_done) {
  if (!on_done) {
    return;
  }
  auto draft = MakeLiveChatAnnounceDraft(topic_id, program_id, join_handle, viewer_peer_id, body);
  if (!draft) {
    on_done(draft.error());
    return;
  }
  PublishAndPushAsync(peer_key, *draft, now_ms, std::move(on_done));
}

Roe<PeerAnnounceTipAck> BroadcastServices::PublishLiveChatFromOverlay(const std::string& peer_key,
                                                                      const std::string& topic_id,
                                                                      const std::string& program_id,
                                                                      const std::string& join_handle,
                                                                      const std::string& viewer_peer_id,
                                                                      const AnnounceOverlayReplyBody& body,
                                                                      const int64_t now_ms) {
  auto draft = MakeLiveChatAnnounceDraft(topic_id, program_id, join_handle, viewer_peer_id, body);
  if (!draft) {
    return draft.error();
  }
  return PublishAndPush(peer_key, *draft, now_ms);
}

std::optional<PeerAnnounceTip> BroadcastServices::LatestTip(const std::string& peer_id, const std::string& topic_id,
                                                            const std::string& program_id) const {
  return feed_.Latest(peer_id, topic_id, program_id);
}

void BroadcastServices::PutLiveProgramKey(const std::string& program_id, const std::string& join_handle,
                                          AmpBroadcastTransport::LiveProgramKey key) {
  tickets_->PutLiveProgramKey(program_id, join_handle, std::move(key));
}

void BroadcastServices::ClearLiveProgramKey(const std::string& program_id, const std::string& join_handle) {
  tickets_->ClearLiveProgramKey(program_id, join_handle);
}

} // namespace pbr
