#include "domain/media/VoiceProcessingIo.h"

#include "domain/media/AudioSpscRing.h"

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <CoreAudio/CoreAudio.h>
#endif

#include <atomic>
#include <cstring>
#include <vector>

namespace pbr {
namespace {

constexpr double kSampleRate = 48000.0;
constexpr size_t kRingSamples = 48000 / 5;  // 200 ms each way
constexpr UInt32 kMaxFramesPerSlice = 4096;
constexpr AudioUnitElement kOutputBus = 0;  // speaker
constexpr AudioUnitElement kInputBus = 1;   // mic

AudioStreamBasicDescription MonoS16() {
  AudioStreamBasicDescription f{};
  f.mSampleRate = kSampleRate;
  f.mFormatID = kAudioFormatLinearPCM;
  f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
  f.mChannelsPerFrame = 1;
  f.mBitsPerChannel = 16;
  f.mBytesPerFrame = 2;
  f.mFramesPerPacket = 1;
  f.mBytesPerPacket = 2;
  return f;
}

bool IsMonoS16(const AudioStreamBasicDescription& f) {
  constexpr AudioFormatFlags kMask =
      kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsFloat | kAudioFormatFlagIsNonInterleaved;
  return f.mSampleRate == kSampleRate && f.mFormatID == kAudioFormatLinearPCM &&
         (f.mFormatFlags & kMask) == kAudioFormatFlagIsSignedInteger && f.mChannelsPerFrame == 1 &&
         f.mBitsPerChannel == 16;
}

std::string Failed(const char* what, OSStatus st) {
  return std::string(what) + " failed (OSStatus " + std::to_string(static_cast<int>(st)) + ")";
}

std::string FormatText(const AudioStreamBasicDescription& f) {
  return "rate=" + std::to_string(static_cast<int>(f.mSampleRate)) + " ch=" + std::to_string(f.mChannelsPerFrame) +
         " bits=" + std::to_string(f.mBitsPerChannel) + " flags=" + std::to_string(f.mFormatFlags);
}

#if TARGET_OS_OSX
constexpr AudioObjectPropertyAddress kDefaultInputAddr = {
    kAudioHardwarePropertyDefaultInputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
constexpr AudioObjectPropertyAddress kDefaultOutputAddr = {
    kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
#endif

}  // namespace

struct VoiceProcessingIo::Impl {
  AudioUnit unit = nullptr;
  AudioSpscRing capture{kRingSamples};
  AudioSpscRing playout{kRingSamples};
  std::vector<int16_t> input_scratch = std::vector<int16_t>(kMaxFramesPerSlice);
  std::atomic<uint64_t> playout_underruns{0};
  std::atomic<bool> device_changed{false};
  bool listening_input = false;
  bool listening_output = false;

  // Real-time thread: no locks, no allocation, no logging.
  static OSStatus OnInput(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* ts, UInt32 bus,
                          UInt32 frames, AudioBufferList* /*unused*/) {
    auto* self = static_cast<Impl*>(ref);
    if (frames > kMaxFramesPerSlice) {
      return kAudio_ParamError;
    }
    AudioBufferList list{};
    list.mNumberBuffers = 1;
    list.mBuffers[0].mNumberChannels = 1;
    list.mBuffers[0].mDataByteSize = frames * sizeof(int16_t);
    list.mBuffers[0].mData = self->input_scratch.data();
    const OSStatus st = AudioUnitRender(self->unit, flags, ts, bus, frames, &list);
    if (st == noErr) {
      self->capture.Write(self->input_scratch.data(), list.mBuffers[0].mDataByteSize / sizeof(int16_t));
    }
    return st;
  }

  static OSStatus OnRender(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* /*ts*/,
                           UInt32 /*bus*/, UInt32 frames, AudioBufferList* io) {
    auto* self = static_cast<Impl*>(ref);
    auto* dst = static_cast<int16_t*>(io->mBuffers[0].mData);
    const size_t want = std::min<size_t>(frames, io->mBuffers[0].mDataByteSize / sizeof(int16_t));
    const size_t got = self->playout.Read(dst, want);
    if (got < want) {
      std::memset(dst + got, 0, (want - got) * sizeof(int16_t));
      self->playout_underruns.fetch_add(1, std::memory_order_relaxed);
      if (got == 0) {
        *flags |= kAudioUnitRenderAction_OutputIsSilence;
      }
    }
    return noErr;
  }

#if TARGET_OS_OSX
  static OSStatus OnDefaultDeviceChanged(AudioObjectID, UInt32, const AudioObjectPropertyAddress*, void* ref) {
    static_cast<Impl*>(ref)->device_changed.store(true, std::memory_order_release);
    return noErr;
  }
#endif
};

VoiceProcessingIo::VoiceProcessingIo() : impl_(std::make_unique<Impl>()) {}

VoiceProcessingIo::~VoiceProcessingIo() {
  Close();
}

bool VoiceProcessingIo::Open(std::string* reason) {
  Close();
  auto fail = [&](std::string why) {
    if (reason) {
      *reason = std::move(why);
    }
    Close();
    return false;
  };

  AudioComponentDescription desc{};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
  AudioComponent comp = AudioComponentFindNext(nullptr, &desc);
  if (!comp) {
    return fail("VoiceProcessingIO component not found");
  }
  OSStatus st = AudioComponentInstanceNew(comp, &impl_->unit);
  if (st != noErr) {
    impl_->unit = nullptr;
    return fail(Failed("AudioComponentInstanceNew", st));
  }
  AudioUnit unit = impl_->unit;

  // Input and output IO are both enabled by default on VPIO; setting EnableIO fails (-10865).
  const AudioStreamBasicDescription fmt = MonoS16();
  st = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, kInputBus, &fmt,
                            sizeof(fmt));
  if (st != noErr) {
    return fail(Failed("set capture format", st));
  }
  st = AudioUnitSetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, kOutputBus, &fmt,
                            sizeof(fmt));
  if (st != noErr) {
    return fail(Failed("set playback format", st));
  }
  const UInt32 max_frames = kMaxFramesPerSlice;
  (void)AudioUnitSetProperty(unit, kAudioUnitProperty_MaximumFramesPerSlice, kAudioUnitScope_Global, 0,
                             &max_frames, sizeof(max_frames));

  AURenderCallbackStruct input_cb{&Impl::OnInput, impl_.get()};
  st = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_SetInputCallback, kAudioUnitScope_Global, kInputBus,
                            &input_cb, sizeof(input_cb));
  if (st != noErr) {
    return fail(Failed("set input callback", st));
  }
  AURenderCallbackStruct render_cb{&Impl::OnRender, impl_.get()};
  st = AudioUnitSetProperty(unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, kOutputBus,
                            &render_cb, sizeof(render_cb));
  if (st != noErr) {
    return fail(Failed("set render callback", st));
  }

#if TARGET_OS_OSX
  // Duck other apps as little as the OS allows while a call runs (macOS 14+).
  if (__builtin_available(macOS 14.0, *)) {
    AUVoiceIOOtherAudioDuckingConfiguration ducking{};
    ducking.mEnableAdvancedDucking = true;
    ducking.mDuckingLevel = kAUVoiceIOOtherAudioDuckingLevelMin;
    (void)AudioUnitSetProperty(unit, kAUVoiceIOProperty_OtherAudioDuckingConfiguration, kAudioUnitScope_Global, 0,
                               &ducking, sizeof(ducking));
  }
#endif

  st = AudioUnitInitialize(unit);
  if (st != noErr) {
    return fail(Failed("AudioUnitInitialize", st));  // e.g. -10875 on mismatched in/out devices
  }

  // The unit may silently substitute its own format (e.g. multi-channel); we only handle mono s16.
  AudioStreamBasicDescription got{};
  UInt32 size = sizeof(got);
  st = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, kInputBus, &got, &size);
  if (st != noErr || !IsMonoS16(got)) {
    return fail("capture format mismatch " + FormatText(got));
  }
  size = sizeof(got);
  st = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, kOutputBus, &got, &size);
  if (st != noErr || !IsMonoS16(got)) {
    return fail("playback format mismatch " + FormatText(got));
  }

  // Neither callback runs yet and the engine does not write playout until Open() returns.
  impl_->capture.Reset();
  impl_->playout.Reset();
  impl_->playout_underruns.store(0, std::memory_order_relaxed);
  impl_->device_changed.store(false, std::memory_order_relaxed);

#if TARGET_OS_OSX
  if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &kDefaultInputAddr, &Impl::OnDefaultDeviceChanged,
                                     impl_.get()) == noErr) {
    impl_->listening_input = true;
  }
  if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &kDefaultOutputAddr, &Impl::OnDefaultDeviceChanged,
                                     impl_.get()) == noErr) {
    impl_->listening_output = true;
  }
#endif

  st = AudioOutputUnitStart(unit);
  if (st != noErr) {
    return fail(Failed("AudioOutputUnitStart", st));
  }
  return true;
}

void VoiceProcessingIo::Close() {
#if TARGET_OS_OSX
  if (impl_->listening_input) {
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &kDefaultInputAddr, &Impl::OnDefaultDeviceChanged,
                                      impl_.get());
    impl_->listening_input = false;
  }
  if (impl_->listening_output) {
    AudioObjectRemovePropertyListener(kAudioObjectSystemObject, &kDefaultOutputAddr, &Impl::OnDefaultDeviceChanged,
                                      impl_.get());
    impl_->listening_output = false;
  }
#endif
  if (impl_->unit) {
    AudioOutputUnitStop(impl_->unit);  // returns after in-flight callbacks finish
    AudioUnitUninitialize(impl_->unit);
    AudioComponentInstanceDispose(impl_->unit);
    impl_->unit = nullptr;
  }
}

bool VoiceProcessingIo::IsOpen() const {
  return impl_->unit != nullptr;
}

size_t VoiceProcessingIo::ReadCapture(int16_t* out, size_t max_samples) {
  return impl_->unit ? impl_->capture.Read(out, max_samples) : 0;
}

size_t VoiceProcessingIo::WritePlayout(const int16_t* pcm, size_t samples) {
  return impl_->unit ? impl_->playout.Write(pcm, samples) : 0;
}

size_t VoiceProcessingIo::QueuedPlayoutBytes() const {
  return impl_->unit ? impl_->playout.Size() * sizeof(int16_t) : 0;
}

bool VoiceProcessingIo::TakeDeviceChanged() {
  return impl_->device_changed.exchange(false, std::memory_order_acq_rel);
}

uint64_t VoiceProcessingIo::PlayoutUnderruns() const {
  return impl_->playout_underruns.load(std::memory_order_relaxed);
}

}  // namespace pbr
