// macOS and iOS: a sound file plays through AVAudioPlayer (MP3, M4A / AAC, WAV, CAF ...). On iOS it uses the app's
// audio session, which the caller puts in playback first (VoiceAudioSession: loudspeaker, plays with the silent
// switch on). Objective-C++ without ARC: retain / release by hand. Called on the main (UI) thread.
#include "lib/voice/AudioFilePlayer.h"

#include "common/Logger.h"
#include "common/PathUtf8.h"
#include "lib/voice/AudioContainerSniff.h"

#include <TargetConditionals.h>

#import <AVFoundation/AVFoundation.h>
#import <Foundation/Foundation.h>

#include <string>
#include <vector>

namespace {

pbr::logging::Logger& Log() {
  static pbr::logging::Logger log = pbr::logging::getLogger("AudioFile");
  return log;
}

/** An NSError for the log: domain, code and the system's text (it names no file). */
std::string DescribeError(NSError* error) {
  if (error == nil) {
    return "none";
  }
  return std::string(error.domain.UTF8String) + "/" + std::to_string(static_cast<long>(error.code)) + " " +
         (error.localizedDescription != nil ? error.localizedDescription.UTF8String : "");
}

/** Where the sound goes, so a log tells a silent play from a failed one: the file's type by its extension, and on
 *  iOS the audio session's category, output port, volume and whether another app plays. No name, no folder. */
std::string DescribeForLog(NSURL* url) {
  std::string line = std::string("ext=") + (url.pathExtension.length > 0 ? url.pathExtension.UTF8String : "-");
#if TARGET_OS_IPHONE
  AVAudioSession* session = [AVAudioSession sharedInstance];
  NSString* out = @"-";
  if (session.currentRoute.outputs.count > 0) {
    out = session.currentRoute.outputs.firstObject.portType;
  }
  line += std::string(" category=") + session.category.UTF8String + " output=" + out.UTF8String +
          " volume=" + std::to_string(static_cast<int>(session.outputVolume * 100)) +
          " other_audio=" + (session.otherAudioPlaying ? "1" : "0");
#endif
  return line;
}

} // namespace

@interface PPAudioFileDelegate : NSObject <AVAudioPlayerDelegate> {
@public
  std::function<void(pbr::VoicePlayEnd)> ended;
}
@end

@implementation PPAudioFileDelegate
- (void)audioPlayerDidFinishPlaying:(AVAudioPlayer*)player successfully:(BOOL)flag {
  Log().info << "sound file ended " << (flag ? "at its end" : "undecodable")
             << " at_ms=" << static_cast<int64_t>(player.currentTime * 1000)
             << " duration_ms=" << static_cast<int64_t>(player.duration * 1000);
  if (ended) {
    auto call = std::move(ended);
    ended = nullptr;
    call(flag ? pbr::VoicePlayEnd::Finished : pbr::VoicePlayEnd::Unplayable);
  }
}
- (void)audioPlayerDecodeErrorDidOccur:(AVAudioPlayer*)player error:(NSError*)error {
  Log().warning << "sound file decode error: " << DescribeError(error);
  if (ended) {
    auto call = std::move(ended);
    ended = nullptr;
    call(pbr::VoicePlayEnd::Unplayable);
  }
}
@end

namespace pbr {
namespace {

// The AVFileTypes to try for a sniffed container, best first (none for Flac / Ogg / Aac / Unknown: no hint).
std::vector<NSString*> TypeHints(AudioContainer container) {
  switch (container) {
    case AudioContainer::Mp3:
      return {AVFileTypeMPEGLayer3};
    case AudioContainer::Mp4:
      return {AVFileTypeAppleM4A, AVFileTypeMPEG4};
    case AudioContainer::Wav:
      return {AVFileTypeWAVE};
    case AudioContainer::Aiff:
      return {AVFileTypeAIFF};
    default:
      return {};
  }
}

const char* ContainerName(AudioContainer container) {
  const char* ext = AudioContainerExtension(container);
  return ext != nullptr ? ext : "unknown";
}

class DarwinAudioFilePlayer final : public IAudioFilePlayer {
public:
  ~DarwinAudioFilePlayer() override { Stop(); }

  bool Play(const std::string& path, AudioFilePlayerCallbacks callbacks) override {
    Stop();
    @autoreleasepool {
      NSURL* url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:path.c_str()]];
      NSError* error = nil;
      // AudioFile picks its parser from the extension: an M4A named .mp3 fails ('dta?'). The content says what the
      // file is, so the player is told that first; the un-hinted open stays as the fallback.
      const AudioContainer container = SniffAudioFile(PathFromUtf8(path));
      AVAudioPlayer* player = nil;
      for (NSString* hint : TypeHints(container)) {
        error = nil;
        player = [[AVAudioPlayer alloc] initWithContentsOfURL:url fileTypeHint:hint error:&error];
        if (player != nil) {
          break;
        }
      }
      if (player == nil) {
        error = nil;
        player = [[AVAudioPlayer alloc] initWithContentsOfURL:url error:&error];
      }
      if (player == nil) {
        Log().warning << "sound file player did not start: cannot open (" << DescribeError(error) << ") "
                      << DescribeForLog(url) << " container=" << ContainerName(container)
                      << " ext=" << PathFromUtf8(path).extension().string();
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
        Log().warning << "sound file player did not start: play refused " << DescribeForLog(url);
        player.delegate = nil;
        [player release];
        [delegate release];
        return false;
      }
      Log().info << "sound file playing duration_ms=" << static_cast<int64_t>(player.duration * 1000)
                 << " sample_rate=" << static_cast<int>(player.format.sampleRate) << " " << DescribeForLog(url);
      player_ = player;
      delegate_ = delegate;
      StartTimer(std::move(callbacks.on_progress));
      return true;
    }
  }

  void Pause() override { [player_ pause]; }

  void Resume() override { [player_ play]; }

  Roe<void> Seek(double seconds) override {
    if (player_ == nil) {
      return Error("no sound file playing");
    }
    // Works playing or paused; AVAudioPlayer clamps nothing itself, so keep it inside the file.
    const double length = player_.duration;
    player_.currentTime = seconds < 0 ? 0 : (length > 0 && seconds > length ? length : seconds);
    return {};
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
