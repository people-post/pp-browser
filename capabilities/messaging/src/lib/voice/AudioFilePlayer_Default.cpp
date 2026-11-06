// Desktop Windows / Linux: no player of our own for sound files; they open in the system's app.
#include "lib/voice/AudioFilePlayer.h"

namespace pbr {

std::unique_ptr<IAudioFilePlayer> CreateSystemAudioFilePlayer() {
  return nullptr;
}

} // namespace pbr
