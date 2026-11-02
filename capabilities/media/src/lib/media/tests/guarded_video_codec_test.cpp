// L016: a hardware codec call never holds its caller past its budget; a codec that stopped
// responding fails at once from then on, and is destroyed on its own thread when it lets go.
#include "lib/media/GuardedVideoCodec.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

namespace pbr {
namespace {

using namespace std::chrono_literals;

struct CodecProbe {
  std::atomic<bool> hang_encode{false};
  std::atomic<bool> release{false};
  std::atomic<bool> configured{false};
  std::atomic<int> encodes{0};
  std::atomic<int> destroyed{0};
};

/** Encodes one byte per frame; an armed Encode hangs until released (bounded: a failing test ends). */
class ProbeCodec final : public IVideoCodec {
public:
  explicit ProbeCodec(CodecProbe& probe) : probe_(probe) {}
  ~ProbeCodec() override { probe_.destroyed.fetch_add(1); }
  std::string BackendName() const override { return "probe"; }
  bool HasEncoder() const override { return probe_.configured.load(); }
  bool HasDecoder() const override { return false; }
  Roe<void> ConfigureEncoder(int, int, int) override {
    probe_.configured.store(true);
    return {};
  }
  Roe<void> ConfigureDecoder() override { return Error("no decoder"); }
  Roe<EncodedAccessUnit> Encode(const VideoFrameI420&, bool) override {
    if (probe_.hang_encode.load()) {
      for (int i = 0; i < 1000 && !probe_.release.load(); ++i) {
        std::this_thread::sleep_for(5ms);
      }
    }
    probe_.encodes.fetch_add(1);
    return EncodedAccessUnit{{0x01}, false};
  }
  Roe<std::optional<VideoFrameRgba>> Decode(const uint8_t*, size_t) override { return Error("no decoder"); }
  void ResetEncoder() override {}
  void ResetDecoder() override {}

private:
  CodecProbe& probe_;
};

VideoCodecBudgets ShortBudgets() {
  VideoCodecBudgets budgets;
  budgets.create = 300ms;
  budgets.configure = 300ms;
  budgets.frame = 150ms;
  budgets.reset = 300ms;
  return budgets;
}

bool WaitFor(const std::function<bool()>& done, std::chrono::milliseconds budget = 3s) {
  const auto deadline = std::chrono::steady_clock::now() + budget;
  while (!done() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(2ms);
  }
  return done();
}

TEST(GuardedVideoCodecTest, CallsGoThroughToTheCodec) {
  CodecProbe probe;
  auto codec = MakeGuardedVideoCodec([&] { return std::make_unique<ProbeCodec>(probe); }, VideoCodecRole::Encoder,
                                     ShortBudgets());
  EXPECT_EQ(codec->BackendName(), "probe");
  EXPECT_FALSE(codec->HasEncoder());
  ASSERT_TRUE(codec->ConfigureEncoder(64, 36, 20));
  EXPECT_TRUE(codec->HasEncoder()) << "known as soon as the call returns";
  auto encoded = codec->Encode(VideoFrameI420{}, false);
  ASSERT_TRUE(encoded);
  EXPECT_EQ(encoded->annex_b.size(), 1u);
  EXPECT_FALSE(codec->NotResponding());
  codec.reset();
  EXPECT_TRUE(WaitFor([&] { return probe.destroyed.load() == 1; }));
}

TEST(GuardedVideoCodecTest, AHungCallReturnsAtItsBudgetAndLaterCallsFailAtOnce) {
  CodecProbe probe;
  auto codec = MakeGuardedVideoCodec([&] { return std::make_unique<ProbeCodec>(probe); }, VideoCodecRole::Encoder,
                                     ShortBudgets());
  ASSERT_TRUE(codec->ConfigureEncoder(64, 36, 20));
  probe.hang_encode.store(true);
  const auto from = std::chrono::steady_clock::now();
  EXPECT_FALSE(codec->Encode(VideoFrameI420{}, false));
  EXPECT_LT(std::chrono::steady_clock::now() - from, 1s) << "waited past the budget";
  EXPECT_TRUE(codec->NotResponding());

  const auto again = std::chrono::steady_clock::now();
  EXPECT_FALSE(codec->Encode(VideoFrameI420{}, false));
  codec->ResetEncoder();
  EXPECT_LT(std::chrono::steady_clock::now() - again, 50ms) << "a codec not responding is not waited on again";

  const auto drop = std::chrono::steady_clock::now();
  codec.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - drop, 50ms) << "dropping it never waits";
  EXPECT_EQ(probe.destroyed.load(), 0) << "still inside the hung call";
  probe.release.store(true);
  EXPECT_TRUE(WaitFor([&] { return probe.destroyed.load() == 1; })) << "destroyed once the call let go";
  EXPECT_EQ(probe.encodes.load(), 1) << "the call queued behind the hang never ran";
}

TEST(GuardedVideoCodecTest, AHungCreateLeavesACodecThatIsNotResponding) {
  std::atomic<bool> release{false};
  CodecProbe probe;
  const auto from = std::chrono::steady_clock::now();
  auto codec = MakeGuardedVideoCodec(
      [&]() -> std::unique_ptr<IVideoCodec> {
        for (int i = 0; i < 1000 && !release.load(); ++i) {
          std::this_thread::sleep_for(5ms);
        }
        return std::make_unique<ProbeCodec>(probe);
      },
      VideoCodecRole::Decoder, ShortBudgets());
  EXPECT_LT(std::chrono::steady_clock::now() - from, 1s);
  EXPECT_TRUE(codec->NotResponding());
  EXPECT_FALSE(codec->ConfigureDecoder());
  codec.reset();
  release.store(true);
  EXPECT_TRUE(WaitFor([&] { return probe.destroyed.load() == 1; })) << "made late, destroyed at once";
}

} // namespace
} // namespace pbr
