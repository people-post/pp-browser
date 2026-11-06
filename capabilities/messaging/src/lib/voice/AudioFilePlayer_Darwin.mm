// macOS and iOS: a sound file plays through AVAudioPlayer (MP3, M4A / AAC, WAV, CAF ...). On iOS it uses the app's
// audio session, which the caller puts in playback first (VoiceAudioSession: loudspeaker, plays with the silent
// switch on). Objective-C++ without ARC: retain / release by hand. Called on the main (UI) thread.
#include "lib/voice/AudioFilePlayer.h"

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

@interface PPAudioFileDelegate : NSObject <AVAudioPlayerDelegate> {
@public
  std::function<void(pbr::VoicePlayEnd)> ended;
}
@end

@implementation PPAudioFileDelegate
- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer*)player successfully:(BOOL)flag {
  if (ended) {
    auto call = std::move(ended);
    ended = nullptr;
    call(flag ? pbr::VoicePlayEnd::Finished : pbr::VoicePlayEnd::Unplayable);
  }
}
- (void)audioPlayerDecodeErrorDidOccur:(AVAudioPlayer*)player error:(NSError*)error {
  if (ended) {
    auto call = std::move(ended);
    ended = nullptr;
    call(pbr::VoicePlayEnd::Unplayable);
  }
}
@end

namespace pbr {
namespace {

class DarwinAudioFilePlayer final : public IAudioFilePlayer {
public:
  ~DarwinAudioFilePlayer() override { Stop(); }

  bool Play(const std::string& path, AudioFilePlayerCallbacks callbacks) override {
    Stop();
    @autoreleasepool {
      NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
      NSError* error = nil;
      AVAudioPlayer* player = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:&error];
      if (player == nil) {
        return false;
      }
      PPAudioFileDelegate* delegate = [[PPAudioFileDelegate alloc] init];
      // The delegate fires on the main thread after Stop() may have run: it ends this Play only (the timer and the
      // player are let go by Stop, which clears the delegate first).
      delegate->ended = [this, ended = callbacks.on_ended](VoicePlayEnd end) {
        StopTimer();
        if (ended) {
          ended(end);
        }
      };
      player.delegate = delegate;
      [player prepareToPlay];
      if (![player play]) {
        player.delegate = nil;
        [player release];
        [delegate release];
        return false;
      }
      player_ = player;
      delegate_ = delegate;
      StartTimer(std::move(callbacks.on_progress));
      return true;
    }
  }

  void Pause() override {
    [player_ pause];
  }

  void Resume() override {
    [player_ play];
  }

  void Stop() override {
    StopTimer();
    if (player_ != nil) {
      player_.delegate = nil;
      [player_ stop];
      [player_ release];
      player_ = nil;
    }
    if (delegate_ != nil) {
      delegate_->ended = nullptr;
      [delegate_ release];
      delegate_ = nil;
    }
  }

private:
  void StartTimer(std::function<void(int64_t, int64_t)> on_progress) {
    if (!on_progress) {
      return;
    }
    timer_ = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
    dispatch_source_set_timer(timer_, dispatch_time(DISPATCH_TIME_NOW, 0), 100 * NSEC_PER_MSEC, 20 * NSEC_PER_MSEC);
    AVAudioPlayer* player = player_;
    dispatch_source_set_event_handler(timer_, ^{
      if (player.playing) {
        on_progress(static_cast<int64_t>(player.currentTime * 1000), static_cast<int64_t>(player.duration * 1000));
      }
    });
    dispatch_resume(timer_);
  }

  void StopTimer() {
    if (timer_ != nullptr) {
      dispatch_source_cancel(timer_);
      dispatch_release(timer_);
      timer_ = nullptr;
    }
  }

  AVAudioPlayer* player_ = nil;
  PPAudioFileDelegate* delegate_ = nil;
  dispatch_source_t timer_ = nullptr;
};

} // namespace

std::unique_ptr<IAudioFilePlayer> CreateSystemAudioFilePlayer() {
  return std::make_unique<DarwinAudioFilePlayer>();
}

} // namespace pbr
