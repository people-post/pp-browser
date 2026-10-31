#include "lib/media/VoiceAudioSession.h"

#include <TargetConditionals.h>

#if defined(__APPLE__) && TARGET_OS_IPHONE

#import <AVFoundation/AVFoundation.h>

namespace pbr {
namespace VoiceAudioSession {
namespace {

void SetCategory(AVAudioSessionCategory category, AVAudioSessionCategoryOptions options) {
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSError* error = nil;
  if (![session setCategory:category mode:AVAudioSessionModeDefault options:options error:&error]) {
    NSLog(@"VoiceAudioSession: setCategory failed: %@", error);
    error = nil;
  }
  if (![session setActive:YES error:&error]) {
    NSLog(@"VoiceAudioSession: setActive failed: %@", error);
  }
}

} // namespace

void ActivateForPlayback() {
  // `playback` ignores the ring/silent switch; no override port is set, so headphones and Bluetooth win over the
  // loudspeaker, and with none the loudspeaker is the route (the phone has no earpiece route for this category).
  SetCategory(AVAudioSessionCategoryPlayback, 0);
}

void ActivateForRecording() {
  SetCategory(AVAudioSessionCategoryPlayAndRecord,
              AVAudioSessionCategoryOptionDefaultToSpeaker | AVAudioSessionCategoryOptionAllowBluetoothA2DP);
}

void RestoreForPlayback() {
  ActivateForPlayback();
}

void Release() {
  NSError* error = nil;
  [[AVAudioSession sharedInstance] setActive:NO
                                 withOptions:AVAudioSessionSetActiveOptionNotifyOthersOnDeactivation
                                       error:&error];
  (void)error; // busy while the device is still closing: the next owner sets its own session
}

} // namespace VoiceAudioSession
} // namespace pbr

#endif
