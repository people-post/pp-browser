#pragma once

#include "lib/media/MediaDeviceArbiter.h"

#include <chrono>
#include <functional>
#include <string>

namespace pbr {

/**
 * Rebuilding SDL audio (SDL_QuitSubSystem + SDL_InitSubSystem of AUDIO), which only Android needs: when the OS
 * disconnects the recording stream before the app closes it, SDL 3.4's AAudio backend marks its only default
 * recording device dead (SDL_aaudio.c AAUDIO_RecordDevice returns -1 without recovering), the default never changes
 * on Android, and every later open of it "succeeds" without opening anything until SDL audio starts again.
 * Elsewhere (SdlAudioReset_Default.cpp) there is nothing to rebuild.
 */
bool SdlAudioResetSupported();

/**
 * Rebuild SDL audio on SDL's main thread (SDL_QuitAudio frees the lock the main thread's event pump reads), waiting
 * at most `budget` for the main thread to get to it. `safe_now` runs on the main thread right before the rebuild:
 * false (an SDL audio stream is open) refuses it (Busy). Timeout when the main thread did not start it in time: it
 * then never runs. Stuck when it started and ran past `budget`: this thread then stays until it returns (nothing may
 * touch SDL audio meanwhile), and no rebuild is tried again in this process (SdlAudioResetSupported turns false).
 * Called on the audio device thread with no endpoint open (AudioResetHandoff holds the rules).
 */
AudioResetResult ResetSdlAudioOnMainThread(std::chrono::milliseconds budget, std::function<bool()> safe_now,
                                           std::string* error);

} // namespace pbr
