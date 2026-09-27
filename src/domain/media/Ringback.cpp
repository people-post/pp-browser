#include "domain/media/Ringback.h"

#include <cmath>

namespace pbr {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kToneHz = 450.0;
constexpr double kBurstSeconds = 1.0;
constexpr double kSilenceSeconds = 4.0;
constexpr double kFadeSeconds = 0.01;
constexpr double kPeakAmplitude = 0.25 * 32767.0;

} // namespace

std::vector<int16_t> MakeRingbackCycle(int sample_rate) {
  const int burst_len = static_cast<int>(std::lround(kBurstSeconds * sample_rate));
  const int silence_len = static_cast<int>(std::lround(kSilenceSeconds * sample_rate));
  const int fade_len = static_cast<int>(std::lround(kFadeSeconds * sample_rate));

  std::vector<int16_t> cycle(static_cast<size_t>(burst_len + silence_len), 0);

  for (int i = 0; i < burst_len; ++i) {
    double envelope = 1.0;
    if (fade_len > 1) {
      if (i < fade_len) {
        envelope = 0.5 * (1.0 - std::cos(kPi * i / (fade_len - 1)));
      } else if (i >= burst_len - fade_len) {
        const int j = burst_len - 1 - i;
        envelope = 0.5 * (1.0 - std::cos(kPi * j / (fade_len - 1)));
      }
    }
    const double sample =
        std::sin(2.0 * kPi * kToneHz * i / sample_rate) * envelope * kPeakAmplitude;
    cycle[static_cast<size_t>(i)] = static_cast<int16_t>(std::lround(sample));
  }

  return cycle;
}

} // namespace pbr
