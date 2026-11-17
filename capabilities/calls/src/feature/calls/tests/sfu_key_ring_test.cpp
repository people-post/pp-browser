// V003: a group call's key rotates on every leave; a hop attach seals with the newest epoch the others hold (V068)
// and opens the previous one only for a short grace.
#include "feature/calls/SfuKeyRing.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

ByteVector KeyOf(uint8_t fill) {
  return ByteVector(32, fill);
}

TEST(SfuKeyRingTest, SealsWithTheNewestAndOpensThePreviousForAGrace) {
  SfuKeyRing ring;
  EXPECT_TRUE(ring.Current(0).key.empty());
  EXPECT_TRUE(ring.ForOpen(0).empty());

  ring.Install(1, KeyOf(1), 1000);
  EXPECT_EQ(ring.Current(1000).epoch, 1u);
  ASSERT_EQ(ring.ForOpen(1000).size(), 1u);

  ring.Install(2, KeyOf(2), 2000);
  EXPECT_EQ(ring.Current(2000).epoch, 2u) << "a key taken from a peer seals at once";
  EXPECT_EQ(ring.Current(2000).key, KeyOf(2));
  auto open = ring.ForOpen(2000 + SfuKeyRing::kPreviousEpochGraceMs - 1);
  ASSERT_EQ(open.size(), 2u);
  EXPECT_EQ(open[0].epoch, 2u) << "the newest first";
  EXPECT_EQ(open[1].epoch, 1u) << "the previous while the others switch";
  EXPECT_EQ(ring.ForOpen(2000 + SfuKeyRing::kPreviousEpochGraceMs).size(), 1u)
      << "after the grace, the leaver's key opens nothing";
}

TEST(SfuKeyRingTest, OnlyMovesForward) {
  SfuKeyRing ring;
  ring.Install(3, KeyOf(3), 0);
  ring.Install(2, KeyOf(2), 10); // a late CallMediaKey for an older epoch
  ring.Install(3, KeyOf(9), 20); // the same epoch again
  EXPECT_EQ(ring.Current(20).epoch, 3u);
  EXPECT_EQ(ring.Current(20).key, KeyOf(3));
  EXPECT_EQ(ring.ForOpen(20).size(), 1u);
}

TEST(SfuKeyRingTest, OurRotationSealsTheOldEpochUntilEachOtherHoldsTheNewOne) {
  SfuKeyRing ring;
  ring.Install(1, KeyOf(1), 0);
  ring.Install(2, KeyOf(2), 1000, {7, 8}); // minted here; streams 7 and 8 are the others
  EXPECT_EQ(ring.Current(1000).epoch, 1u) << "nobody else holds epoch 2 yet";
  ASSERT_EQ(ring.ForOpen(1000).size(), 2u) << "a frame sealed with the new key opens at once";
  ring.NoteOpened(7, 1, 1100); // the old epoch says nothing
  ring.NoteOpened(7, 2, 1200);
  EXPECT_EQ(ring.Current(1200).epoch, 1u) << "stream 8 has not shown it holds the new key";
  ring.NoteOpened(8, 2, 1300);
  EXPECT_EQ(ring.Current(1300).epoch, 2u) << "everyone holds it: the rotation is complete";
  EXPECT_EQ(ring.ForOpen(1300 + SfuKeyRing::kPreviousEpochGraceMs).size(), 1u)
      << "the old epoch opens for its grace after we stopped sealing with it, then never";
}

TEST(SfuKeyRingTest, OurRotationSealsTheNewEpochAtTheBoundWhateverTheOthersShowed) {
  SfuKeyRing ring;
  ring.Install(1, KeyOf(1), 0);
  ring.Install(2, KeyOf(2), 1000, {7}); // stream 7 never got the key (or died)
  EXPECT_EQ(ring.Current(1000 + SfuKeyRing::kSealHoldMaxMs - 1).epoch, 1u);
  EXPECT_EQ(ring.Current(1000 + SfuKeyRing::kSealHoldMaxMs).epoch, 2u)
      << "the one who left must not read our media for longer than the bound";
}

TEST(SfuKeyRingTest, ANewerRotationDuringAHoldReplacesIt) {
  SfuKeyRing ring;
  ring.Install(1, KeyOf(1), 0);
  ring.Install(2, KeyOf(2), 1000, {7, 8});
  ring.Install(3, KeyOf(3), 1500, {7}); // someone else left meanwhile
  ring.NoteOpened(7, 2, 1600);
  EXPECT_EQ(ring.Current(1600).epoch, 1u) << "epoch 2 is not what we wait for any more";
  ASSERT_EQ(ring.ForOpen(1600).size(), 3u) << "frames under either new epoch open";
  ring.NoteOpened(7, 3, 1700);
  EXPECT_EQ(ring.Current(1700).epoch, 3u);
}

// Three devices on one hop, A rotates when D leaves (V003): A mints epoch 2, its CallMediaKey reaches B after
// `to_b_ms` and C after `to_c_ms`. Each sends a frame every 40 ms; a frame reaches the others 30 ms later and opens
// only under a key the receiver holds then. Counts the frames that did not open, per sender → receiver.
struct RotationRun {
  std::map<std::pair<char, char>, int> unopened;
  int Total() const {
    int n = 0;
    for (const auto& [_, count] : unopened) {
      n += count;
    }
    return n;
  }
};

RotationRun RunRotation(const int64_t to_b_ms, const int64_t to_c_ms) {
  constexpr int64_t kRotateAt = 10'000;
  constexpr int64_t kTransitMs = 30;
  constexpr int64_t kFrameMs = 40;
  const std::map<char, uint32_t> stream{{'A', 1}, {'B', 2}, {'C', 3}};
  std::map<char, std::unique_ptr<SfuKeyRing>> rings;
  for (const auto& [who, _] : stream) {
    rings[who] = std::make_unique<SfuKeyRing>();
    rings[who]->Install(1, KeyOf(1), 0);
  }
  const std::map<char, int64_t> key_at{{'A', kRotateAt}, {'B', kRotateAt + to_b_ms}, {'C', kRotateAt + to_c_ms}};
  std::map<char, bool> installed;
  struct InFlight {
    int64_t arrives;
    char from;
    char to;
    uint32_t epoch;
  };
  std::vector<InFlight> flight;
  RotationRun run;
  for (int64_t now = 0; now <= kRotateAt + 10'000; now += 10) {
    for (const auto& [who, at] : key_at) {
      if (!installed[who] && now >= at) {
        installed[who] = true;
        // The minter holds for the others' streams; a peer installs what it was sent.
        const std::vector<uint32_t> hold = who == 'A' ? std::vector<uint32_t>{2, 3} : std::vector<uint32_t>{};
        rings[who]->Install(2, KeyOf(2), now, hold);
      }
    }
    for (auto it = flight.begin(); it != flight.end();) {
      if (it->arrives > now) {
        ++it;
        continue;
      }
      const std::vector<SfuKeyRing::Key> keys = rings[it->to]->ForOpen(now);
      const bool opens =
          std::any_of(keys.begin(), keys.end(), [&](const SfuKeyRing::Key& k) { return k.epoch == it->epoch; });
      if (opens) {
        rings[it->to]->NoteOpened(stream.at(it->from), it->epoch, now);
      } else {
        ++run.unopened[{it->from, it->to}];
      }
      it = flight.erase(it);
    }
    if (now % kFrameMs == 0) {
      for (const auto& [from, _] : stream) {
        const uint32_t epoch = rings[from]->Current(now).epoch;
        for (const auto& [to, __] : stream) {
          if (to != from) {
            flight.push_back({now + kTransitMs, from, to, epoch});
          }
        }
      }
    }
  }
  return run;
}

TEST(SfuKeyRingTest, ARotationCostsTheOthersNoFramesOfTheMinters) {
  // #617's follow-up: the key took ~1.2 s to reach the others, and the minter sealed with it at once.
  const RotationRun run = RunRotation(/*to_b_ms=*/1200, /*to_c_ms=*/1250);
  EXPECT_EQ((run.unopened.count({'A', 'B'}) ? run.unopened.at({'A', 'B'}) : 0), 0)
      << "the minter waits until B holds the new key";
  EXPECT_EQ((run.unopened.count({'A', 'C'}) ? run.unopened.at({'A', 'C'}) : 0), 0)
      << "the minter waits until C holds the new key";
  // B seals with the key when it lands, 50 ms before it reaches C: at most that gap's frames, and the frame on the way.
  EXPECT_LE(run.Total(), 3) << "only the skew between the others' copies of the key";
}

TEST(SfuKeyRingTest, AMemberWhoNeverGetsTheKeyCostsTheOthersNothingAndHoldsTheMinterOnlyToTheBound) {
  const RotationRun run = RunRotation(/*to_b_ms=*/1200, /*to_c_ms=*/60'000); // C missed it (a resend comes later)
  EXPECT_EQ((run.unopened.count({'A', 'B'}) ? run.unopened.at({'A', 'B'}) : 0), 0);
  EXPECT_GT((run.unopened.count({'A', 'C'}) ? run.unopened.at({'A', 'C'}) : 0), 0)
      << "after the bound the minter seals with the new key: C, without it, is cut off until the key reaches it";
}

} // namespace
} // namespace pbr
