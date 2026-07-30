#include "base/media/CallAudioSession.h"

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <AVFoundation/AVFoundation.h>

namespace pbr {
namespace CallAudioSession {

void ActivateForVoipCall() {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  AVAudioSessionCategoryOptions options =
      AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetooth;
  [session setCategory:AVAudioSessionCategoryPlayAndRecord withOptions:options error:&error];
  [session setMode:AVAudioSessionModeVoiceChat error:&error];
  [session setActive:YES error:&error];
  (void)error;
}

void Deactivate() {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  [session setActive:NO withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation error:&error];
  (void)error;
}

} // namespace CallAudioSession
} // namespace pbr

#endif
