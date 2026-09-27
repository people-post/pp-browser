#include "domain/media/Ringback.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace pbr {
namespace {

constexpr int kSampleRate = 24000;

TEST(RingbackTest, CycleLengthIsFiveSecondsAtGivenRate) {
  const auto cycle = MakeRingbackCycle(kSampleRate);
  EXPECT_EQ(cycle.size(), static_cast<size_t>(5 * kSampleRate));
}

TEST(RingbackTest, FadesStartAndEndAtZero) {
  const auto cycle = MakeRingbackCycle(kSampleRate);
  const size_t burst_len = static_cast<size_t>(kSampleRate); // 1.0 s
  EXPECT_EQ(cycle[0], 0);
  EXPECT_EQ(cycle[burst_len - 1], 0);
}

TEST(RingbackTest, PeakAmplitudeMatchesMinus12Dbfs) {
  const auto cycle = MakeRingbackCycle(kSampleRate);
  const size_t burst_len = static_cast<size_t>(kSampleRate);
  const double expected_peak = 0.25 * 32767.0;
  int16_t max_abs = 0;
  for (size_t i = 0; i < burst_len; ++i) {
    max_abs = std::max<int16_t>(max_abs, static_cast<int16_t>(std::abs(cycle[i])));
  }
  EXPECT_LE(max_abs, expected_peak + 1);
  EXPECT_GE(max_abs, 0.24 * 32767.0);
}

TEST(RingbackTest, ZeroCrossingsMatch450HzOverFirstSecond) {
  const auto cycle = MakeRingbackCycle(kSampleRate);
  const size_t burst_len = static_cast<size_t>(kSampleRate); // == 1.0 s at this rate
  // Quantization to int16 rounds some near-zero samples to exactly 0 (not just a sign
  // flip), so compare each nonzero sample against the last nonzero sign seen rather than
  // adjacent samples directly.
  int crossings = 0;
  int last_sign = 0;
  for (size_t i = 0; i < burst_len; ++i) {
    const int sign = cycle[i] > 0 ? 1 : (cycle[i] < 0 ? -1 : 0);
    if (sign != 0) {
      if (last_sign != 0 && sign != last_sign) {
        ++crossings;
      }
      last_sign = sign;
    }
  }
  EXPECT_NEAR(crossings, 2 * 450, 4);
}

TEST(RingbackTest, SilenceForRemainingFourSeconds) {
  const auto cycle = MakeRingbackCycle(kSampleRate);
  const size_t burst_len = static_cast<size_t>(kSampleRate);
  for (size_t i = burst_len; i < cycle.size(); ++i) {
    ASSERT_EQ(cycle[i], 0) << "nonzero silence sample at index " << i;
  }
}

} // namespace
} // namespace pbr
