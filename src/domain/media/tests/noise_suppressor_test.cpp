#include "domain/media/NoiseSuppressor.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <random>
#include <stdexcept>
#include <vector>

namespace pbr {
namespace {

constexpr double kPi = 3.14159265358979323846;

std::vector<int16_t> WhiteNoise(size_t n, double amplitude_dbfs, uint32_t seed) {
  std::mt19937 rng(seed);
  const double amp = 32767.0 * std::pow(10.0, amplitude_dbfs / 20.0);
  std::uniform_real_distribution<double> uni(-1.0, 1.0);
  std::vector<int16_t> out(n);
  for (size_t i = 0; i < n; ++i) {
    out[i] = static_cast<int16_t>(std::lround(uni(rng) * amp));
  }
  return out;
}

std::vector<int16_t> Sine(size_t n, double hz, double amplitude_dbfs) {
  const double amp = 32767.0 * std::pow(10.0, amplitude_dbfs / 20.0);
  std::vector<int16_t> out(n);
  for (size_t i = 0; i < n; ++i) {
    out[i] = static_cast<int16_t>(std::lround(amp * std::sin(2.0 * kPi * hz * static_cast<double>(i) /
                                                             NoiseSuppressor::kSampleRate)));
  }
  return out;
}

double RmsDb(const int16_t* pcm, size_t n) {
  double acc = 0.0;
  for (size_t i = 0; i < n; ++i) {
    acc += static_cast<double>(pcm[i]) * pcm[i];
  }
  const double rms = std::sqrt(acc / static_cast<double>(n)) / 32767.0;
  return 20.0 * std::log10(std::max(rms, 1e-9));
}

// Run `ns` over `in` in 960-sample chunks (like the capture loop); return the processed copy.
std::vector<int16_t> RunInFrames(NoiseSuppressor& ns, std::vector<int16_t> in) {
  constexpr size_t kFrame = 960;
  for (size_t off = 0; off + kFrame <= in.size(); off += kFrame) {
    ns.Process(in.data() + off, kFrame);
  }
  return in;
}

TEST(NoiseSuppressorTest, RejectsNon48k) {
  EXPECT_THROW(NoiseSuppressor ns(44100), std::invalid_argument);
}

TEST(NoiseSuppressorTest, ZeroInZeroOut) {
  NoiseSuppressor ns;
  std::vector<int16_t> pcm(960 * 10, 0);
  auto out = RunInFrames(ns, pcm);
  for (int16_t s : out) {
    EXPECT_EQ(s, 0);
  }
}

TEST(NoiseSuppressorTest, ConstantDelayIsKLatencySamples) {
  // An impulse must come out exactly kLatencySamples later (after the window has settled).
  NoiseSuppressor ns;
  std::vector<int16_t> pcm(960 * 4, 0);
  pcm[960] = 20000;
  auto out = RunInFrames(ns, pcm);
  size_t argmax = 0;
  for (size_t i = 0; i < out.size(); ++i) {
    if (std::abs(out[i]) > std::abs(out[argmax])) {
      argmax = i;
    }
  }
  EXPECT_EQ(argmax, 960 + NoiseSuppressor::kLatencySamples);
}

TEST(NoiseSuppressorTest, WhiteNoiseAttenuatedAtLeast10dB) {
  NoiseSuppressor ns;
  const size_t n = 48000 * 3; // 3 s
  auto in = WhiteNoise(n, -40.0, 7);
  auto out = RunInFrames(ns, in);
  // Compare the last second (after convergence).
  const double in_db = RmsDb(in.data() + 48000 * 2, 48000);
  const double out_db = RmsDb(out.data() + 48000 * 2, 48000);
  EXPECT_LE(out_db, in_db - 10.0) << "in=" << in_db << " out=" << out_db;
  EXPECT_LT(ns.last_noise_floor_dbfs(), -30.0);
  EXPECT_GT(ns.last_noise_floor_dbfs(), -60.0);
}

TEST(NoiseSuppressorTest, SinePreservedNoiseReduced) {
  NoiseSuppressor ns;
  const size_t n = 48000 * 3;
  // 1 s noise only (learn floor), then 2 s sine + noise.
  auto noise = WhiteNoise(n, -40.0, 11);
  auto sine = Sine(n, 1000.0, -20.0);
  std::vector<int16_t> in(n);
  for (size_t i = 0; i < n; ++i) {
    const int s = (i >= 48000 ? sine[i] : 0) + noise[i];
    in[i] = static_cast<int16_t>(std::max(-32768, std::min(32767, s)));
  }
  auto out = RunInFrames(ns, in);
  // Project the last second of output onto the delayed clean sine.
  const size_t d = NoiseSuppressor::kLatencySamples;
  double num = 0.0, den = 0.0;
  for (size_t i = 48000 * 2 + d; i < n; ++i) {
    num += static_cast<double>(out[i]) * sine[i - d];
    den += static_cast<double>(sine[i - d]) * sine[i - d];
  }
  const double gain = num / den;
  EXPECT_GT(gain, 0.95) << "sine gain " << gain;
  EXPECT_LT(gain, 1.05) << "sine gain " << gain;
}

} // namespace
} // namespace pbr
