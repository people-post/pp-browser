#include "lib/media/VoiceSupport.h"

namespace pbr {

bool VoiceMessagesSupported() {
  return false; // no microphone button, no player: a voice message is a file row
}

} // namespace pbr
