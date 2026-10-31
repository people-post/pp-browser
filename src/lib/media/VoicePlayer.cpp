#include "lib/media/VoicePlayer.h"

#include "common/Logger.h"
#include "lib/media/VoiceAudioSession.h"
#include "lib/media/VoiceRecorder.h"
#include "lib/media/VoiceSupport.h"

#include <opus.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace pbr {
namespace {

logging::Logger& PlayerLog() {
  static logging::Logger log = logging::getLogger("VoicePlayer");
  return log;
}

constexpr int kChunkSamples = kOggOpusFrameSamples;  // 20 ms
constexpr int kChunkBytes = kChunkSamples * static_cast<int>(sizeof(int16_t));
/** Keep about 100 ms queued in the device. */
constexpr int kQueueTargetBytes = 5 * kChunkBytes;
constexpr int kMaxDecodedFrameSamples = 5760; // 120 ms, the longest Opus packet

class ArbiterVoicePlaybackSink final : public IVoicePlaybackSink {
public:
  explicit ArbiterVoicePlaybackSink(MediaDeviceArbiter& devices) : devices_(devices) {}
  ~ArbiterVoicePlaybackSink() override { Close(); }

  Roe<void> Open(const std::function<bool()>& still_wanted) override {
    // Not during a call: its holder (the call id, or "ringtone" / "ringback") has the speaker, and a recording
    // has the microphone. Shared speakers would mix, which is what a call must not get.
    if (const std::string by = Blocker(); !by.empty()) {
      return Error(kOpenBusy, "in use by " + by);
    }
    AudioLeaseRequest request;
    request.kind = MediaDeviceKind::Speaker;
    request.holder = kVoicePlayerHolder;
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
    VoiceAudioSession::ActivateForPlayback(); // after the open: SDL sets the session itself when it opens a device
    return {};
  }

  int Queued() const override { return lease_ ? lease_->Queued() : 0; }
  bool Write(const void* src, const int bytes) override { return lease_ && lease_->Write(src, bytes); }
  void Clear() override {
    if (lease_) {
      lease_->Clear();
    }
  }
  std::string InterruptedBy() override { return Blocker(); }

  void Close() override {
    if (!lease_) {
      return;
    }
    lease_->Clear();
    lease_.reset();
    if (VoiceDeviceBlockedBy(devices_).empty()) { // a call that took the devices meanwhile owns the audio session
      VoiceAudioSession::Release();
    }
  }

private:
  std::string Blocker() const {
    for (const auto& holder : devices_.Holders(MediaDeviceKind::Speaker)) {
      if (holder != kVoicePlayerHolder) {
        return holder;
      }
    }
    for (const auto& holder : devices_.Holders(MediaDeviceKind::Mic)) {
      if (holder != kVoicePlayerHolder) {
        return holder;
      }
    }
    return {};
  }

  MediaDeviceArbiter& devices_;
  std::unique_ptr<AudioDeviceLease> lease_;
};

/** The samples of the clip after the pre-skip and up to the end granule; nullopt when a packet does not decode. */
bool DecodeStream(const OggOpusStream& stream, std::vector<int16_t>* pcm) {
  int err = OPUS_OK;
  OpusDecoder* decoder = opus_decoder_create(kOggOpusRate, 1, &err);
  if (!decoder || err != OPUS_OK) {
    return false;
  }
  std::vector<int16_t> all;
  all.reserve(stream.packets.size() * kOggOpusFrameSamples);
  std::vector<int16_t> frame(kMaxDecodedFrameSamples);
  bool ok = true;
  for (const auto& packet : stream.packets) {
    const int n = opus_decode(decoder, packet.data(), static_cast<opus_int32>(packet.size()), frame.data(),
                              kMaxDecodedFrameSamples, 0);
    if (n < 0) {
      ok = false;
      break;
    }
    all.insert(all.end(), frame.begin(), frame.begin() + n);
  }
  opus_decoder_destroy(decoder);
  if (!ok || all.size() <= static_cast<size_t>(stream.pre_skip)) {
    return false;
  }
  const size_t begin = static_cast<size_t>(stream.pre_skip);
  const size_t end = std::min<size_t>(all.size(), begin + static_cast<size_t>(stream.valid_samples));
  pcm->assign(all.begin() + static_cast<std::ptrdiff_t>(begin), all.begin() + static_cast<std::ptrdiff_t>(end));
  return !pcm->empty();
}

} // namespace

std::unique_ptr<IVoicePlaybackSink> CreateArbiterVoicePlaybackSink(MediaDeviceArbiter& devices) {
  return std::make_unique<ArbiterVoicePlaybackSink>(devices);
}

VoicePlayer::VoicePlayer(SinkFactory factory, VoicePlayerCallbacks callbacks)
    : factory_(std::move(factory)), callbacks_(std::move(callbacks)) {}

VoicePlayer::~VoicePlayer() {
  Stop();
  std::lock_guard lock(mutex_);
  if (thread_.joinable()) {
    thread_.join();
  }
}

void VoicePlayer::Play(const std::string& id, const std::string& path, const int max_seconds) {
  std::lock_guard lock(mutex_);
  if (job_) {
    job_->stop.store(true);
  }
  if (thread_.joinable()) {
    thread_.join(); // the clip before reports Stopped here, before the new one starts
  }
  paused_.store(false);
  job_ = std::make_shared<Job>();
  job_->id = id;
  job_->path = path;
  job_->max_seconds = max_seconds;
  thread_ = std::thread([this, job = job_]() { Run(job); });
}

void VoicePlayer::Pause() {
  paused_.store(true);
}

void VoicePlayer::Resume() {
  paused_.store(false);
}

void VoicePlayer::Stop() {
  std::lock_guard lock(mutex_);
  if (job_) {
    job_->stop.store(true);
  }
}

std::string VoicePlayer::CurrentId() const {
  std::lock_guard lock(mutex_);
  return job_ && !job_->stop.load() && !job_->done.load() ? job_->id : std::string();
}

void VoicePlayer::Run(std::shared_ptr<Job> job) {
  using Clock = std::chrono::steady_clock;
  const auto end_with = [&](const VoicePlayEnd end) {
    job->done.store(true);
    if (callbacks_.on_ended) {
      callbacks_.on_ended(job->id, end);
    }
  };

  // Parsed here, on the first tap and not before: the file is from someone else.
  OggOpusLimits limits;
  limits.max_samples = (static_cast<uint64_t>(std::max(job->max_seconds, 1)) + 1) * kOggOpusRate;
  std::vector<uint8_t> bytes;
  {
    // Size first: a file longer than a voice message can be is never read into memory.
    std::error_code size_ec;
    const auto file_size = std::filesystem::file_size(job->path, size_ec);
    if (size_ec || file_size > limits.max_file_bytes) {
      end_with(VoicePlayEnd::Unplayable);
      return;
    }
    std::ifstream in(job->path, std::ios::binary);
    if (!in) {
      end_with(VoicePlayEnd::Unplayable);
      return;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  }
  auto parsed = ParseOggOpus(bytes.data(), bytes.size(), limits);
  std::vector<int16_t> pcm;
  if (!parsed || !DecodeStream(*parsed, &pcm)) {
    PlayerLog().info << "voice file not playable: " << (parsed ? "decode failed" : parsed.error().message);
    end_with(VoicePlayEnd::Unplayable);
    return;
  }
  bytes.clear();
  bytes.shrink_to_fit();
  const int64_t duration_ms = static_cast<int64_t>(pcm.size() * 1000 / kOggOpusRate);

  auto sink = factory_ ? factory_() : nullptr;
  if (!sink) {
    end_with(VoicePlayEnd::NoDevice);
    return;
  }
  auto opened = sink->Open([&job]() { return !job->stop.load(); });
  if (!opened) {
    end_with(opened.error().code == IVoicePlaybackSink::kOpenBusy ? VoicePlayEnd::Busy : VoicePlayEnd::NoDevice);
    return;
  }

  if (callbacks_.on_progress) {
    callbacks_.on_progress(job->id, 0, duration_ms);
  }
  size_t written = 0; // samples handed to the sink
  bool paused = false;
  auto last_progress = Clock::now();
  auto last_move = Clock::now(); // the sink's queue last got shorter (or we last wrote)
  int last_queued = -1;
  VoicePlayEnd end = VoicePlayEnd::Finished;
  const auto position_samples = [&]() {
    const size_t queued = static_cast<size_t>(std::max(sink->Queued(), 0)) / sizeof(int16_t);
    return written > queued ? written - queued : size_t{0};
  };

  for (;;) {
    if (job->stop.load()) {
      end = VoicePlayEnd::Stopped;
      break;
    }
    if (!sink->InterruptedBy().empty()) {
      end = VoicePlayEnd::Interrupted;
      break;
    }
    const bool want_pause = paused_.load();
    if (want_pause != paused) {
      if (want_pause) {
        written = position_samples(); // resume from where it was heard, not from what was queued
        sink->Clear();
      }
      paused = want_pause;
    }
    if (paused) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
      continue;
    }

    if (callbacks_.on_progress && Clock::now() - last_progress >= std::chrono::milliseconds(100)) {
      last_progress = Clock::now();
      callbacks_.on_progress(job->id, static_cast<int64_t>(position_samples() * 1000 / kOggOpusRate), duration_ms);
    }
    const int queued = std::max(sink->Queued(), 0);
    if (queued != last_queued) {
      if (queued < last_queued) {
        last_move = Clock::now();
      }
      last_queued = queued;
    }
    if (written < pcm.size()) {
      if (queued < kQueueTargetBytes) {
        const size_t n = std::min<size_t>(kChunkSamples, pcm.size() - written);
        if (!sink->Write(pcm.data() + written, static_cast<int>(n * sizeof(int16_t)))) {
          end = VoicePlayEnd::NoDevice;
          break;
        }
        written += n;
        last_move = Clock::now();
        continue;
      }
    } else if (queued <= 0 || Clock::now() - last_move > std::chrono::seconds(3)) {
      end = VoicePlayEnd::Finished;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (end == VoicePlayEnd::Finished) {
    if (callbacks_.on_progress) {
      callbacks_.on_progress(job->id, duration_ms, duration_ms);
    }
  } else {
    sink->Clear();
  }
  sink->Close();
  end_with(end);
}

} // namespace pbr
