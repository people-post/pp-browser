#include "domain/media/VoiceProcessingIo.h"

#include "domain/media/AudioSpscRing.h"

#include <AudioToolbox/AudioToolbox.h>
#include <TargetConditionals.h>
#if TARGET_OS_OSX
#include <CoreAudio/CoreAudio.h>
#else
#include "domain/media/CallAudioSession.h"
#endif

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

namespace pbr {
namespace {

constexpr double kSampleRate = 48000.0;
constexpr size_t kRingSamples = 48000 / 5;  // 200 ms each way
constexpr UInt32 kMaxFramesPerSlice = 4096;
#if TARGET_OS_OSX
/** Ask for a 10 ms hardware IO buffer; VPIO on macOS otherwise ran 4096-frame (85 ms) cycles.
 *  (iOS sets the IO buffer on AVAudioSession instead.) */
constexpr UInt32 kPreferredIoFrames = 480;
#endif
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

// Non-allocating; a fixed-size property read. Safe on the capture thread (TakeDeviceChanged)
// and inside Open()/its failure path — never called from the AudioUnit render callbacks.
void QueryDefaultDevices(AudioDeviceID* in, AudioDeviceID* out) {
  *in = kAudioObjectUnknown;
  *out = kAudioObjectUnknown;
  UInt32 size = sizeof(AudioDeviceID);
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &kDefaultInputAddr, 0, nullptr, &size, in);
  size = sizeof(AudioDeviceID);
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &kDefaultOutputAddr, 0, nullptr, &size, out);
}

// Best effort: ask the unit (AUHAL forwards it to the device), then the device itself, for a small
// IO buffer. Returns the buffer size in effect afterwards (0 if it cannot be read).
UInt32 RequestIoBufferFrames(AudioUnit unit, UInt32 frames) {
  UInt32 want = frames;
  (void)AudioUnitSetProperty(unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &want,
                             sizeof(want));
  UInt32 now = 0;
  UInt32 size = sizeof(now);
  if (AudioUnitGetProperty(unit, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &now, &size) ==
          noErr &&
      now <= frames * 2) {
    return now;
  }
  AudioDeviceID device = kAudioObjectUnknown;
  size = sizeof(device);
  if (AudioUnitGetProperty(unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device,
                           &size) != noErr ||
      device == kAudioObjectUnknown) {
    return now;
  }
  const AudioObjectPropertyAddress addr = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal,
                                           kAudioObjectPropertyElementMain};
  want = frames;
  (void)AudioObjectSetPropertyData(device, &addr, 0, nullptr, sizeof(want), &want);
  size = sizeof(now);
  if (AudioObjectGetPropertyData(device, &addr, 0, nullptr, &size, &now) != noErr) {
    now = 0;
  }
  return now;
}
#endif

}  // namespace


struct VoiceProcessingIo::Impl {
  AudioUnit unit = nullptr;
  AudioSpscRing capture{kRingSamples};
  AudioSpscRing playout{kRingSamples};
  std::vector<int16_t> input_scratch = std::vector<int16_t>(kMaxFramesPerSlice);
  std::atomic<uint64_t> playout_underruns{0};
  // Diagnostics (single writer: the callback that owns them; TakeDiag resets — races are benign).
  std::atomic<uint64_t> render_calls{0};
  std::atomic<uint32_t> render_frames_max{0};
  /** Largest render request since Open() (not reset by TakeDiag) — the engine sizes playout to it. */
  std::atomic<uint32_t> render_chunk_max{0};
  std::atomic<uint32_t> io_buffer_frames{0};
  std::atomic<size_t> render_ring_min{SIZE_MAX};
  std::atomic<uint64_t> input_calls{0};
  std::atomic<uint32_t> input_frames_max{0};
  std::atomic<size_t> capture_ring_max{0};
  std::atomic<bool> device_changed{false};
  /** Set once WritePlayout() has delivered real samples since the last Open(); OnRender only
   *  counts an underrun after that (silence before the engine starts writing isn't one). */
  std::atomic<bool> playout_started{false};
  bool listening_input = false;
  bool listening_output = false;
  /** Listeners are registered once, on the object's first Open() attempt (success or
   *  failure), and removed only in the destructor — see I3 in the final-review notes. */
  bool listener_registration_attempted = false;
#if TARGET_OS_OSX
  /** Default in/out device recorded at the end of every Open() attempt; TakeDeviceChanged()
   *  compares against these so a notification caused by Open() itself (or a route flap right
   *  after Start) does not look like a real device change. */
  AudioDeviceID baseline_input = kAudioObjectUnknown;
  AudioDeviceID baseline_output = kAudioObjectUnknown;
#endif

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
    self->input_calls.fetch_add(1, std::memory_order_relaxed);
    if (frames > self->input_frames_max.load(std::memory_order_relaxed)) {
      self->input_frames_max.store(frames, std::memory_order_relaxed);
    }
    const size_t cap = self->capture.Size();
    if (cap > self->capture_ring_max.load(std::memory_order_relaxed)) {
      self->capture_ring_max.store(cap, std::memory_order_relaxed);
    }
    return st;
  }

  static OSStatus OnRender(void* ref, AudioUnitRenderActionFlags* flags, const AudioTimeStamp* /*ts*/,
                           UInt32 /*bus*/, UInt32 frames, AudioBufferList* io) {
    auto* self = static_cast<Impl*>(ref);
    auto* dst = static_cast<int16_t*>(io->mBuffers[0].mData);
    const size_t want = std::min<size_t>(frames, io->mBuffers[0].mDataByteSize / sizeof(int16_t));
    self->render_calls.fetch_add(1, std::memory_order_relaxed);
    if (frames > self->render_frames_max.load(std::memory_order_relaxed)) {
      self->render_frames_max.store(frames, std::memory_order_relaxed);
    }
    if (frames > self->render_chunk_max.load(std::memory_order_relaxed)) {
      self->render_chunk_max.store(frames, std::memory_order_relaxed);
    }
    const size_t avail = self->playout.Size();
    if (avail < self->render_ring_min.load(std::memory_order_relaxed)) {
      self->render_ring_min.store(avail, std::memory_order_relaxed);
    }
    const size_t got = self->playout.Read(dst, want);
    if (got < want) {
      std::memset(dst + got, 0, (want - got) * sizeof(int16_t));
      if (self->playout_started.load(std::memory_order_acquire)) {
        self->playout_underruns.fetch_add(1, std::memory_order_relaxed);
      }
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
#if TARGET_OS_OSX
  // Listeners are registered once (first Open() attempt) and live for the object's lifetime
  // (I3): remove them here, not in Close(), so a later SDL-fallback call still notices a real
  // default-device change and retries VPIO.
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
}

bool VoiceProcessingIo::Open(std::string* reason) {
  Close();
  // Clear before recording a fresh baseline below (I1/I3): a stale flag from before this
  // attempt must not immediately look like a "device changed since Open()".
  impl_->device_changed.store(false, std::memory_order_relaxed);

#if TARGET_OS_OSX
  if (!impl_->listener_registration_attempted) {
    impl_->listener_registration_attempted = true;
    if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &kDefaultInputAddr, &Impl::OnDefaultDeviceChanged,
                                       impl_.get()) == noErr) {
      impl_->listening_input = true;
    }
    if (AudioObjectAddPropertyListener(kAudioObjectSystemObject, &kDefaultOutputAddr, &Impl::OnDefaultDeviceChanged,
                                       impl_.get()) == noErr) {
      impl_->listening_output = true;
    }
  }
#endif

  auto fail = [&](std::string why) {
    if (reason) {
      *reason = std::move(why);
    }
#if TARGET_OS_OSX
    // Record the baseline even on failure (I1): otherwise a notification fired by this failed
    // attempt (or by whatever caused it to fail) would look like a device change on the very
    // next TakeDeviceChanged(), while we are actually sitting on SDL.
    QueryDefaultDevices(&impl_->baseline_input, &impl_->baseline_output);
#endif
    Close();
    return false;
  };

  AudioComponentDescription desc{};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_VoiceProcessingIO;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;
#if !TARGET_OS_OSX
  // Re-assert the call session here, on the device thread, right before the unit is built: a
  // ringback / ringtone SDL stream closed just ahead of us in the device queue rewrites the
  // AVAudioSession on close (B36), and VPIO then failed AudioOutputUnitStart with 'what'.
  CallAudioSession::ActivateForVoipCall();
#endif
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

  // macOS: input and output IO are both enabled by default on VPIO; setting EnableIO fails (-10865).
  // iOS: like RemoteIO, the input element is disabled by default — without this the unit opens but
  // never calls the input callback (on device: capture starved on every open, then SDL fallback).
#if !TARGET_OS_OSX
  const UInt32 enable = 1;
  st = AudioUnitSetProperty(unit, kAudioOutputUnitProperty_EnableIO, kAudioUnitScope_Input, kInputBus, &enable,
                            sizeof(enable));
  if (st != noErr) {
    return fail(Failed("enable input", st));
  }
#endif
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
#if TARGET_OS_OSX
  impl_->io_buffer_frames.store(RequestIoBufferFrames(unit, kPreferredIoFrames), std::memory_order_relaxed);
#endif

  // The unit may silently substitute its own format (e.g. multi-channel); we only handle mono s16.
  AudioStreamBasicDescription got{};
  UInt32 size = sizeof(got);
  st = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, kInputBus, &got, &size);
  if (st != noErr) {
    return fail(Failed("get capture format", st));
  }
  if (!IsMonoS16(got)) {
    return fail("capture format mismatch " + FormatText(got));
  }
  size = sizeof(got);
  st = AudioUnitGetProperty(unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, kOutputBus, &got, &size);
  if (st != noErr) {
    return fail(Failed("get playback format", st));
  }
  if (!IsMonoS16(got)) {
    return fail("playback format mismatch " + FormatText(got));
  }

  // Neither callback runs yet and the engine does not write playout until Open() returns.
  impl_->capture.Reset();
  impl_->playout.Reset();
  impl_->playout_underruns.store(0, std::memory_order_relaxed);
  impl_->playout_started.store(false, std::memory_order_relaxed);
  impl_->render_chunk_max.store(0, std::memory_order_relaxed);

  st = AudioOutputUnitStart(unit);
  if (st != noErr) {
    // One retry: a session/route change settling right now (e.g. the previous stream's close) is
    // the usual cause of a transient start failure.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    st = AudioOutputUnitStart(unit);
  }
  if (st != noErr) {
    return fail(Failed("AudioOutputUnitStart", st));
  }
#if TARGET_OS_OSX
  // Baseline for TakeDeviceChanged (I1): a notification fired by Start itself (or an
  // AirPods A2DP→HFP flip right after) must not look like a device change on the next check.
  QueryDefaultDevices(&impl_->baseline_input, &impl_->baseline_output);
#endif
  return true;
}

void VoiceProcessingIo::Close() {
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
  if (!impl_->unit) {
    return 0;
  }
  if (samples > 0) {
    impl_->playout_started.store(true, std::memory_order_release);
  }
  return impl_->playout.Write(pcm, samples);
}

size_t VoiceProcessingIo::QueuedPlayoutBytes() const {
  return impl_->unit ? impl_->playout.Size() * sizeof(int16_t) : 0;
}

bool VoiceProcessingIo::TakeDeviceChanged() {
  if (!impl_->device_changed.exchange(false, std::memory_order_acq_rel)) {
    return false;
  }
#if TARGET_OS_OSX
  // A notification does not by itself mean the pair we're bound to changed (I1): confirm
  // against the baseline recorded at the last Open() attempt before triggering a reopen.
  AudioDeviceID in = kAudioObjectUnknown;
  AudioDeviceID out = kAudioObjectUnknown;
  QueryDefaultDevices(&in, &out);
  if (in == impl_->baseline_input && out == impl_->baseline_output) {
    return false;
  }
  // Adopt the new pair now: when the engine reopens on SDL without calling Open() (VPIO disabled
  // for the call) a stale baseline would report every later notification as a change.
  impl_->baseline_input = in;
  impl_->baseline_output = out;
  return true;
#else
  return true;
#endif
}

size_t VoiceProcessingIo::RenderChunkBytes() const {
  return impl_->render_chunk_max.load(std::memory_order_relaxed) * sizeof(int16_t);
}

uint64_t VoiceProcessingIo::PlayoutUnderruns() const {
  return impl_->playout_underruns.load(std::memory_order_relaxed);
}

std::string VoiceProcessingIo::TakeDiag() {
  Impl& d = *impl_;
  const size_t ring_min = d.render_ring_min.exchange(SIZE_MAX, std::memory_order_relaxed);
  return "io_buffer_frames=" + std::to_string(d.io_buffer_frames.load(std::memory_order_relaxed)) +
         " render_calls=" + std::to_string(d.render_calls.exchange(0, std::memory_order_relaxed)) +
         " render_frames_max=" + std::to_string(d.render_frames_max.exchange(0, std::memory_order_relaxed)) +
         " render_ring_min=" + (ring_min == SIZE_MAX ? std::string("-") : std::to_string(ring_min)) +
         " input_calls=" + std::to_string(d.input_calls.exchange(0, std::memory_order_relaxed)) +
         " input_frames_max=" + std::to_string(d.input_frames_max.exchange(0, std::memory_order_relaxed)) +
         " capture_ring_max=" + std::to_string(d.capture_ring_max.exchange(0, std::memory_order_relaxed));
}

}  // namespace pbr
