#include "feature/product/ProductStack.h"

#include "feature/conversations/ConversationsFacade.h"
#include "foundation/data/PlatformDefaults.h"
#include "foundation/platform/ImageThumbnail.h"
#include "foundation/platform/PlatformOpenFile.h"
#include "foundation/platform/Platform.h"
#include "foundation/platform/VideoPosterExtractor.h"
#include "foundation/platform/os/OsDownloadMark.h"
#include "lib/media/AudioSession.h"
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
      .image_thumbnail_jpeg = [](const std::vector<uint8_t>& image_bytes, int max_dimension) {
        return MakeImageThumbnailJpeg(image_bytes, max_dimension);
      },
      .forget_opened_copies = [] { PlatformForgetOpenedFiles(); },
  });
  // A sound or video shown in the app plays like a voice message: on the loudspeaker, also with the silent switch on
  // (never while a call holds the session).
  SetPlatformOpenFileSoundHooks([] { AudioSession::ActivateForVoicePlayback(); }, [] { AudioSession::ReleaseVoice(); });
  hub_->SetMeshOwner(&mesh_);
  hub_->SetHost({.mobile = Platform::IsMobile(), .desktop = Platform::IsDesktop()});
  // An empty endpoint in the profile's config falls back to the platform's defaults (the app's to know).
  const AppConfig defaults = PlatformDefaults::For(Platform::Detect());
  hub_->SetBackendDefaults({defaults.relay, defaults.directory, defaults.registration});
  hub_->SetStorage(&OpenSqliteProfileStores);
  hub_->SetContentStorage(&OpenSqliteObjectIndex);
  // G022: a group invite made during a call reaches us from someone we are in that call with (calls' to say).
  hub_->SetInLiveCallQuery([this](const std::string& call_id, const std::string& inviter) {
    return calls_.InLiveCallWith(call_id, inviter);
  });
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
