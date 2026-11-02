#include "feature/product/ProductStack.h"

#include "feature/conversations/ConversationsFacade.h"
#include "foundation/data/PlatformDefaults.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/VideoPosterExtractor.h"
#include "foundation/platform/os/OsDownloadMark.h"
#include "lib/messaging/AttachmentPlatform.h"
#include "lib/storage/ProfileStorage.h"

#include "common/PbrCompat.h"

namespace pbr {

ProductStack::ProductStack() : hub_(std::make_unique<ConversationsHub>()), calls_(mesh_), broadcast_(*hub_, mesh_) {
  // What the attachment cache asks of the OS: the platform's (quarantine mark, video posters).
  SetAttachmentPlatform({
      .mark_downloaded = [](const std::string& path) { return os::MarkFileAsDownloaded(path); },
      .video_poster_jpeg = [](const std::string& path, int max_dimension) {
        return ExtractVideoPosterJpeg(path, max_dimension);
      },
      .soft_poster_jpeg = [](int max_dimension) { return SoftVideoPosterJpeg(max_dimension); },
  });
  hub_->SetMeshOwner(&mesh_);
  hub_->SetHost({.mobile = Platform::IsMobile(), .desktop = Platform::IsDesktop()});
  // An empty endpoint in the profile's config falls back to the platform's defaults (the app's to know).
  const AppConfig defaults = PlatformDefaults::For(Platform::Detect());
  hub_->SetBackendDefaults({defaults.relay, defaults.directory, defaults.registration});
  hub_->SetStorage(&OpenSqliteProfileStores);
  // Order matters: up in this order (calls, then broadcast), down in reverse.
  hub_->AddParticipant(calls_);
  hub_->AddParticipant(broadcast_);
}

ProductStack::~ProductStack() {
  hub_->Shutdown();
  hub_->ClearParticipants();
}

void ProductStack::BindFacade(ConversationsFacade& facade) {
  facade.SetActiveCallQuery([this](const Thread& thread) { return calls_.ThreadInActiveCall(thread); });
  facade.SetAnyCallQuery([this]() { return calls_.AnyCallLive(); });
}

MessagingPeoplePickerPorts ProductStack::PeoplePickerPorts() {
  MessagingPeoplePickerPorts ports = MakeMessagingPeoplePickerPorts(*hub_);
  ports.list_call_participants = [this](const std::string& call_id) { return calls_.JoinedParticipants(call_id); };
  return ports;
}

} // namespace pbr
