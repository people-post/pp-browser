#include "lib/media/MediaDeviceArbiter.h"
#include "lib/voice/VoicePlayer.h"
#include "lib/voice/VoiceRecorder.h"
#include "lib/voice/VoiceSupport.h"

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
