#include "lib/media/GuardedVideoCodec.h"

#include "common/Diag.h"
#include "common/Logger.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace pbr {
namespace {

logging::Logger& CodecLog() {
  static logging::Logger log = logging::getLogger("VideoCodec");
  return log;
}

ShareWord RoleWord(VideoCodecRole role) {
  switch (role) {
    case VideoCodecRole::Encoder:
      return "encoder";
    case VideoCodecRole::Decoder:
      return "decoder";
  }
  return "codec";
}

const char* RoleName(VideoCodecRole role) {
  return role == VideoCodecRole::Encoder ? "video encoder" : "video decoder";
}

enum class CodecOp : uint8_t { Create, ConfigureEncoder, ConfigureDecoder, Encode, Decode, SetBitrate, Reset };

ShareWord OpWord(CodecOp op) {
  switch (op) {
    case CodecOp::Create:
      return "create";
    case CodecOp::ConfigureEncoder:
      return "configure_encoder";
    case CodecOp::ConfigureDecoder:
      return "configure_decoder";
    case CodecOp::Encode:
      return "encode";
    case CodecOp::Decode:
      return "decode";
    case CodecOp::SetBitrate:
      return "set_bitrate";
    case CodecOp::Reset:
      return "reset";
  }
  return "other";
}

/** The codec and the one thread that touches it; outlives the guard while a call is stuck. */
struct CodecWorker {
  std::mutex mu;
  std::condition_variable cv;
  std::deque<std::function<void()>> tasks;
  bool stopping = false;
  /** Worker thread only. */
  std::unique_ptr<IVideoCodec> codec;
  std::atomic<bool> has_encoder{false};
  std::atomic<bool> has_decoder{false};

  void Run() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mu);
        cv.wait(lock, [this]() { return stopping || !tasks.empty(); });
        if (tasks.empty()) {
          break;
        }
        task = std::move(tasks.front());
        tasks.pop_front();
      }
      task();
    }
    codec.reset(); // here, whenever a stuck call let go
  }

  /** Worker only: what the holder may ask without waiting (HasEncoder / HasDecoder). */
  void Refresh() {
    if (codec) {
      has_encoder.store(codec->HasEncoder(), std::memory_order_release);
      has_decoder.store(codec->HasDecoder(), std::memory_order_release);
    }
  }

  void Post(std::function<void()> task) {
    std::lock_guard lock(mu);
    tasks.push_back(std::move(task));
    cv.notify_one();
  }

  void Stop() {
    std::lock_guard lock(mu);
    stopping = true;
    cv.notify_one();
  }
};

class GuardedVideoCodec final : public IVideoCodec {
public:
  GuardedVideoCodec(std::function<std::unique_ptr<IVideoCodec>()> make, VideoCodecRole role, VideoCodecBudgets budgets)
      : worker_(std::make_shared<CodecWorker>()), role_(role), budgets_(budgets) {
    std::thread([worker = worker_]() { worker->Run(); }).detach();
    struct Facts {
      std::string backend;
      bool encoder_supported = false;
    };
    auto made = Call<Facts>(CodecOp::Create, budgets_.create, [make = std::move(make)](CodecWorker& worker) {
      worker.codec = make ? make() : nullptr;
      Facts facts;
      if (worker.codec) {
        facts.backend = worker.codec->BackendName();
        facts.encoder_supported = worker.codec->EncoderSupported();
      }
      return facts;
    });
    if (made) {
      backend_ = made->backend;
      encoder_supported_ = made->encoder_supported;
    }
  }

  ~GuardedVideoCodec() override { worker_->Stop(); }

  std::string BackendName() const override { return backend_; }
  bool HasEncoder() const override { return worker_->has_encoder.load(std::memory_order_acquire); }
  bool HasDecoder() const override { return worker_->has_decoder.load(std::memory_order_acquire); }
  bool EncoderSupported() const override { return encoder_supported_; }
  bool NotResponding() const override { return hung_.load(std::memory_order_acquire); }

  Roe<void> ConfigureEncoder(int width, int height, int fps) override {
    return Unwrap(Call<Roe<void>>(CodecOp::ConfigureEncoder, budgets_.configure, [=](CodecWorker& worker) -> Roe<void> {
      return worker.codec ? worker.codec->ConfigureEncoder(width, height, fps) : Error("no video codec");
    }));
  }

  Roe<void> ConfigureDecoder() override {
    return Unwrap(Call<Roe<void>>(CodecOp::ConfigureDecoder, budgets_.configure, [](CodecWorker& worker) -> Roe<void> {
      return worker.codec ? worker.codec->ConfigureDecoder() : Error("no video codec");
    }));
  }

  Roe<EncodedAccessUnit> Encode(const VideoFrameI420& frame, bool force_keyframe) override {
    // The frame is copied into the task: a wait that runs out returns while the codec may still read it.
    return Unwrap(Call<Roe<EncodedAccessUnit>>(
        CodecOp::Encode, budgets_.frame, [frame, force_keyframe](CodecWorker& worker) -> Roe<EncodedAccessUnit> {
          return worker.codec ? worker.codec->Encode(frame, force_keyframe) : Error("no video codec");
        }));
  }

  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t* annex_b, size_t size) override {
    std::vector<uint8_t> bytes(annex_b, annex_b + size);
    return Unwrap(Call<Roe<std::optional<VideoFrameRgba>>>(
        CodecOp::Decode, budgets_.frame,
        [bytes = std::move(bytes)](CodecWorker& worker) -> Roe<std::optional<VideoFrameRgba>> {
          return worker.codec ? worker.codec->Decode(bytes.data(), bytes.size()) : Error("no video codec");
        }));
  }

  void SetTargetBitrate(int64_t bps) override {
    (void)Call<bool>(CodecOp::SetBitrate, budgets_.reset, [bps](CodecWorker& worker) {
      if (worker.codec) {
        worker.codec->SetTargetBitrate(bps);
      }
      return true;
    });
  }

  void ResetEncoder() override {
    (void)Call<bool>(CodecOp::Reset, budgets_.reset, [](CodecWorker& worker) {
      if (worker.codec) {
        worker.codec->ResetEncoder();
      }
      return true;
    });
  }

  void ResetDecoder() override {
    (void)Call<bool>(CodecOp::Reset, budgets_.reset, [](CodecWorker& worker) {
      if (worker.codec) {
        worker.codec->ResetDecoder();
      }
      return true;
    });
  }

private:
  /**
   * Run `fn` on the worker and wait at most `budget`. Nothing when it ran out (the codec is now not
   * responding; `fn` still runs later, alone with what it captured) or already was.
   */
  template <class T, class Fn>
  std::optional<T> Call(CodecOp op, std::chrono::milliseconds budget, Fn fn) {
    if (hung_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }
    auto result = std::make_shared<std::promise<T>>();
    auto future = result->get_future();
    worker_->Post([worker = worker_.get(), result, fn = std::move(fn)]() mutable {
      T value = fn(*worker);
      worker->Refresh();
      result->set_value(std::move(value));
    });
    if (future.wait_for(budget) == std::future_status::ready) {
      return future.get();
    }
    hung_.store(true, std::memory_order_release);
    CodecLog().warning << RoleName(role_) << " not responding: " << OpWord(op).c_str() << " took over "
                       << budget.count() << "ms backend=" << backend_;
    DiagLine("media.codec_stuck").Add("role", RoleWord(role_)).Add("op", OpWord(op)).Add("ms", budget).Emit();
    return std::nullopt;
  }

  template <class R>
  R Unwrap(std::optional<R> result) const {
    if (result) {
      return std::move(*result);
    }
    return Error(std::string(RoleName(role_)) + " not responding");
  }

  std::shared_ptr<CodecWorker> worker_;
  VideoCodecRole role_;
  VideoCodecBudgets budgets_;
  std::string backend_;
  bool encoder_supported_ = false;
  std::atomic<bool> hung_{false};
};

} // namespace

std::unique_ptr<IVideoCodec> MakeGuardedVideoCodec(std::function<std::unique_ptr<IVideoCodec>()> make,
                                                   VideoCodecRole role, VideoCodecBudgets budgets) {
  return std::make_unique<GuardedVideoCodec>(std::move(make), role, budgets);
}

} // namespace pbr
