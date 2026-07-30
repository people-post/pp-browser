#include "base/media/CallAudioSession.h"

#if !defined(__APPLE__) || !TARGET_OS_IPHONE

namespace pbr {
namespace CallAudioSession {

void ActivateForVoipCall() {}
void Deactivate() {}

} // namespace CallAudioSession
} // namespace pbr

#endif
