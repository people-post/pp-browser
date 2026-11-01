#include "lib/media/MediaDeviceArbiter.h"
#include "lib/media/VoicePlayer.h"
#include "lib/media/VoiceRecorder.h"
#include "lib/media/VoiceSupport.h"

namespace pbr {

std::string VoiceDeviceBlockedBy(MediaDeviceArbiter& devices) {
  for (const MediaDeviceKind kind : {MediaDeviceKind::Mic, MediaDeviceKind::Speaker}) {
    for (const auto& holder : devices.Holders(kind)) {
      if (holder != kVoiceRecorderHolder && holder != kVoicePlayerHolder) {
        return holder;
      }
    }
  }
  return {};
}

} // namespace pbr
