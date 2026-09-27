#include "domain/media/AudioSpscRing.h"

#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

namespace pbr {
namespace {

TEST(AudioSpscRingTest, StartsEmpty) {
  AudioSpscRing ring(8);
  int16_t out[4] = {};
  EXPECT_EQ(ring.Size(), 0u);
  EXPECT_EQ(ring.Capacity(), 8u);
  EXPECT_EQ(ring.Read(out, 4), 0u);
}

TEST(AudioSpscRingTest, ReadsBackWhatWasWritten) {
  AudioSpscRing ring(8);
  const int16_t in[5] = {1, 2, 3, 4, 5};
  EXPECT_EQ(ring.Write(in, 5), 5u);
  EXPECT_EQ(ring.Size(), 5u);
  int16_t out[5] = {};
  EXPECT_EQ(ring.Read(out, 5), 5u);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(out[i], in[i]);
  }
  EXPECT_EQ(ring.Size(), 0u);
}

TEST(AudioSpscRingTest, PartialReadLeavesRest) {
  AudioSpscRing ring(8);
  const int16_t in[6] = {1, 2, 3, 4, 5, 6};
  ring.Write(in, 6);
  int16_t out[4] = {};
  EXPECT_EQ(ring.Read(out, 4), 4u);
  EXPECT_EQ(out[3], 4);
  EXPECT_EQ(ring.Read(out, 4), 2u);
  EXPECT_EQ(out[0], 5);
  EXPECT_EQ(out[1], 6);
}

TEST(AudioSpscRingTest, WrapsAround) {
  AudioSpscRing ring(5);
  const int16_t a[4] = {1, 2, 3, 4};
  const int16_t b[4] = {5, 6, 7, 8};
  int16_t out[4] = {};
  ring.Write(a, 4);
  ASSERT_EQ(ring.Read(out, 3), 3u);  // leaves {4}
  ASSERT_EQ(ring.Write(b, 4), 4u);   // wraps: {4,5,6,7,8}
  int16_t all[5] = {};
  ASSERT_EQ(ring.Read(all, 5), 5u);
  const int16_t want[5] = {4, 5, 6, 7, 8};
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(all[i], want[i]);
  }
}

TEST(AudioSpscRingTest, DropsWhenFull) {
  AudioSpscRing ring(4);
  const int16_t in[6] = {1, 2, 3, 4, 5, 6};
  EXPECT_EQ(ring.Write(in, 6), 4u);  // 5, 6 dropped
  EXPECT_EQ(ring.Write(in, 1), 0u);
  int16_t out[4] = {};
  ASSERT_EQ(ring.Read(out, 4), 4u);
  EXPECT_EQ(out[0], 1);
  EXPECT_EQ(out[3], 4);
}

TEST(AudioSpscRingTest, ResetEmpties) {
  AudioSpscRing ring(4);
  const int16_t in[3] = {1, 2, 3};
  ring.Write(in, 3);
  ring.Reset();
  EXPECT_EQ(ring.Size(), 0u);
  int16_t out[1] = {};
  EXPECT_EQ(ring.Read(out, 1), 0u);
}

TEST(AudioSpscRingTest, ConcurrentProducerConsumerPreservesOrder) {
  AudioSpscRing ring(1024);
  constexpr int kTotal = 200000;
  std::thread producer([&] {
    int next = 0;
    int16_t chunk[441];  // odd chunk size, like a 44.1 kHz device callback
    while (next < kTotal) {
      const int n = std::min<int>(441, kTotal - next);
      for (int i = 0; i < n; ++i) {
        chunk[i] = static_cast<int16_t>((next + i) & 0x7fff);
      }
      size_t done = 0;
      while (done < static_cast<size_t>(n)) {
        done += ring.Write(chunk + done, static_cast<size_t>(n) - done);
      }
      next += n;
    }
  });
  int expected = 0;
  int16_t buf[960];
  bool ordered = true;
  while (expected < kTotal) {
    const size_t got = ring.Read(buf, 960);
    for (size_t i = 0; i < got; ++i) {
      if (buf[i] != static_cast<int16_t>(expected & 0x7fff)) {
        ordered = false;
      }
      ++expected;
    }
  }
  producer.join();
  EXPECT_TRUE(ordered);
  EXPECT_EQ(ring.Size(), 0u);
}

}  // namespace
}  // namespace pbr
