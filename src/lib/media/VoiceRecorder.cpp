#include "lib/media/VoiceRecorder.h"

#include "common/Logger.h"
#include "lib/media/OggOpus.h"
#include "lib/media/VoiceAudioSession.h"
#include "lib/media/VoiceFile.h"
#include "lib/media/VoiceSupport.h"

#include <opus.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace pbr {
namespace {

logging::Logger& RecorderLog() {
  static logging::Logger log = logging::getLogger("VoiceRecorder");
  return log;
}

constexpr int kFrameBytes = kOggOpusFrameSamples * static_cast<int>(sizeof(int16_t));
constexpr int kProgressEveryFrames = 5; // 100 ms

/** The microphone lease; refused (busy) when a call holds the microphone. */
class ArbiterVoiceCaptureSource final : public IVoiceCaptureSource {
public:
  explicit ArbiterVoiceCaptureSource(MediaDeviceArbiter& devices) : devices_(devices) {}
  ~ArbiterVoiceCaptureSource() override { Close(); }

  Roe<void> Open(const std::function<bool()>& still_wanted) override {
    AudioLeaseRequest request;
    request.kind = MediaDeviceKind::Mic;
    request.holder = kVoiceRecorderHolder;
    request.format = AudioDeviceFormat{kOggOpusRate, 1};
    request.still_wanted = still_wanted;
    auto lease = devices_.AcquireAudio(request);
    if (!lease) {
      return Error(kOpenBusy, lease.error().message);
    }
    if (!(*lease)->HasDevice()) {
      return Error(kOpenNoDevice, (*lease)->OpenError());
    }
    lease_ = std::move(*lease);
    VoiceAudioSession::ActivateForRecording(); // after the open: SDL sets the session itself when it opens a device
    return {};
  }

  int Read(void* dst, const int bytes) override { return lease_ ? lease_->Read(dst, bytes) : -1; }

  std::string InterruptedBy() override {
    // A ringing call takes the speaker (ringtone) before it can take the microphone: stop now so the
    // call can start. The recorder holds only the microphone, so any speaker holder is someone else.
    const auto holders = devices_.Holders(MediaDeviceKind::Speaker);
    for (const auto& holder : holders) {
      if (holder != kVoicePlayerHolder) {
        return holder;
      }
    }
    return {};
  }

  void Close() override {
    if (!lease_) {
      return;
    }
    lease_.reset();
    // Back to the playback setup and handed back to the system, unless a call (or its ringtone) has taken the
    // devices meanwhile: the audio session is then theirs and is not touched.
    if (VoiceDeviceBlockedBy(devices_).empty()) {
      VoiceAudioSession::RestoreForPlayback();
      VoiceAudioSession::Release();
    }
  }

private:
  MediaDeviceArbiter& devices_;
  std::unique_ptr<AudioDeviceLease> lease_;
};

} // namespace

std::unique_ptr<IVoiceCaptureSource> CreateArbiterVoiceCaptureSource(MediaDeviceArbiter& devices) {
  return std::make_unique<ArbiterVoiceCaptureSource>(devices);
}

VoiceRecorder::VoiceRecorder(SourceFactory factory, Callbacks callbacks)
    : factory_(std::move(factory)), callbacks_(std::move(callbacks)) {}

VoiceRecorder::~VoiceRecorder() {
  Cancel();
  std::lock_guard lock(mutex_);
  if (thread_.joinable()) {
    thread_.join();
  }
}

bool VoiceRecorder::Start(const VoiceRecorderConfig& config) {
  std::lock_guard lock(mutex_);
  if (running_.load()) {
    return false;
  }
  if (thread_.joinable()) {
    thread_.join(); // the previous run is over (running_ is false): only its callback may still be returning
  }
  request_ = std::make_shared<std::atomic<int>>(static_cast<int>(Request::None));
  running_.store(true);
  thread_ = std::thread([this, config, request = request_]() { Run(config, request); });
  return true;
}

void VoiceRecorder::Finish() {
  std::lock_guard lock(mutex_);
  if (request_ && running_.load()) {
    int expected = static_cast<int>(Request::None);
    request_->compare_exchange_strong(expected, static_cast<int>(Request::Finish));
  }
}

void VoiceRecorder::Cancel() {
  std::lock_guard lock(mutex_);
  if (request_ && running_.load()) {
    request_->store(static_cast<int>(Request::Cancel)); // wins over a Finish that came first
  }
}

void VoiceRecorder::Run(VoiceRecorderConfig config, std::shared_ptr<std::atomic<int>> request) {
  using Clock = std::chrono::steady_clock;
  const auto requested = [&request]() { return static_cast<Request>(request->load()); };

  VoiceRecording result;
  const auto deliver = [&]() {
    running_.store(false);
    if (callbacks_.on_finished) {
      callbacks_.on_finished(std::move(result));
    }
  };

  if (config.out_dir.empty()) {
    result.end = VoiceEnd::Failed; // a recording is never written to a folder nobody chose
    result.error = "no folder for the recording";
    deliver();
    return;
  }
  auto source = factory_ ? factory_() : nullptr;
  if (!source) {
    result.end = VoiceEnd::NoDevice;
    result.error = "no capture source";
    deliver();
    return;
  }
  auto opened = source->Open([&]() { return requested() == Request::None; });
  if (!opened) {
    result.end = opened.error().code == IVoiceCaptureSource::kOpenBusy ? VoiceEnd::Busy : VoiceEnd::NoDevice;
    result.error = opened.error().message;
    deliver();
    return;
  }

  int err = OPUS_OK;
  OpusEncoder* encoder = opus_encoder_create(kOggOpusRate, 1, OPUS_APPLICATION_VOIP, &err);
  if (!encoder || err != OPUS_OK) {
    source->Close();
    result.end = VoiceEnd::Failed;
    result.error = "opus_encoder_create failed";
    deliver();
    return;
  }
  opus_encoder_ctl(encoder, OPUS_SET_BITRATE(config.bitrate_bps > 0 ? config.bitrate_bps : 24000));
  opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(6));
  opus_int32 lookahead = 0;
  opus_encoder_ctl(encoder, OPUS_GET_LOOKAHEAD(&lookahead));

  const uint64_t max_samples = static_cast<uint64_t>(std::max(config.max_seconds, 1)) * kOggOpusRate;
  std::vector<std::vector<uint8_t>> packets;
  std::array<int16_t, kOggOpusFrameSamples> frame{};
  int filled_bytes = 0;
  uint64_t samples = 0; // real samples captured
  bool failed = false;
  VoiceEnd end = VoiceEnd::Sent;
  auto last_data = Clock::now();
  int frames_since_progress = 0;

  const auto encode_frame = [&]() {
    std::array<unsigned char, 1275> out{};
    const int bytes = opus_encode(encoder, frame.data(), kOggOpusFrameSamples, out.data(), static_cast<opus_int32>(out.size()));
    if (bytes < 0) {
      failed = true;
      return;
    }
    packets.emplace_back(out.begin(), out.begin() + bytes);
  };

  for (;;) {
    const Request req = requested();
    if (req == Request::Cancel) {
      end = VoiceEnd::Cancelled;
      break;
    }
    if (req == Request::Finish) {
      end = VoiceEnd::Sent;
      break;
    }
    if (const std::string by = source->InterruptedBy(); !by.empty()) {
      RecorderLog().info << "recording stopped for " << by;
      end = VoiceEnd::Interrupted;
      break;
    }
    const int n = source->Read(reinterpret_cast<uint8_t*>(frame.data()) + filled_bytes, kFrameBytes - filled_bytes);
    if (n < 0) {
      failed = true;
      break;
    }
    if (n == 0) {
      if (Clock::now() - last_data > std::chrono::milliseconds(config.no_data_timeout_ms)) {
        failed = true;
        result.error = "the microphone gave no sound";
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      continue;
    }
    last_data = Clock::now();
    filled_bytes += n;
    if (filled_bytes < kFrameBytes) {
      continue;
    }
    filled_bytes = 0;
    samples += kOggOpusFrameSamples;
    encode_frame();
    if (failed) {
      break;
    }
    if (++frames_since_progress >= kProgressEveryFrames) {
      frames_since_progress = 0;
      if (callbacks_.on_progress) {
        callbacks_.on_progress(static_cast<int64_t>(samples * 1000 / kOggOpusRate));
      }
    }
    if (samples >= max_samples) {
      end = VoiceEnd::LimitReached;
      break;
    }
  }
  source->Close(); // the microphone is free from here: a call that interrupted us can take it

  if (failed) {
    opus_encoder_destroy(encoder);
    result.end = VoiceEnd::Failed;
    if (result.error.empty()) {
      result.error = "encoding failed";
    }
    deliver();
    return;
  }
  if (end == VoiceEnd::Cancelled) {
    opus_encoder_destroy(encoder);
    result.end = VoiceEnd::Cancelled;
    deliver();
    return;
  }

  // A part-filled last frame counts for what it holds; the rest of it is silence.
  if (filled_bytes > 0) {
    samples += static_cast<uint64_t>(filled_bytes / static_cast<int>(sizeof(int16_t)));
    std::memset(reinterpret_cast<uint8_t*>(frame.data()) + filled_bytes, 0, static_cast<size_t>(kFrameBytes - filled_bytes));
    encode_frame();
  }
  result.duration_ms = static_cast<int64_t>(samples * 1000 / kOggOpusRate);
  if (!failed && result.duration_ms >= config.min_ms) {
    // The encoder delays its output by `lookahead` samples; silent frames flush the tail so the end is not cut.
    std::fill(frame.begin(), frame.end(), 0);
    while (!failed && packets.size() * kOggOpusFrameSamples < samples + static_cast<uint64_t>(lookahead)) {
      encode_frame();
    }
  }
  opus_encoder_destroy(encoder);

  if (failed) {
    result.end = VoiceEnd::Failed;
    result.error = "encoding failed";
    deliver();
    return;
  }
  if (result.duration_ms < config.min_ms) {
    result.end = VoiceEnd::TooShort;
    result.duration_ms = 0;
    deliver();
    return;
  }

  OggOpusWriteParams params;
  params.pre_skip = lookahead;
  const std::vector<uint8_t> file = BuildOggOpus(packets, samples, params);
  std::error_code ec;
  const std::filesystem::path dir = config.out_dir;
  std::filesystem::create_directories(dir, ec);
  const std::filesystem::path path = dir / VoiceFileName(result.duration_ms);
  {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size()));
    out.flush();
    if (!out) {
      out.close();
      std::filesystem::remove(path, ec);
      result.end = VoiceEnd::Failed;
      result.error = "the recording could not be written";
      deliver();
      return;
    }
  }
  result.end = end;
  result.path = path.string();
  deliver();
}

} // namespace pbr
