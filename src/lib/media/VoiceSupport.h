#pragma once

#include <string>

namespace pbr {

/**
 * Whether this build records and plays voice messages. False on Linux: there is no microphone button and a
 * received voice message is a plain file row. True on iOS, Android, macOS and Windows.
 */
bool VoiceMessagesSupported();

/** Who has the microphone or speaker that a voice message would need (a call, a ringtone), or empty. */
class MediaDeviceArbiter;
std::string VoiceDeviceBlockedBy(MediaDeviceArbiter& devices);

} // namespace pbr
