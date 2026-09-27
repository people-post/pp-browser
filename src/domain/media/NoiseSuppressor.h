#pragma once

#include <array>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace pbr {

/**
 * Self-written stationary-noise suppressor for the call capture path (spec §3, no deps).
 * 512-point STFT, sqrt-Hann analysis/synthesis, 50 % overlap; per-bin noise floor by
 * minimum tracking gated on a-posteriori SNR; decision-directed a-priori SNR; Wiener gain
 * with a -18 dB floor, 3-tap smoothing across bins. Constant delay kLatencySamples.
 * 48 kHz mono s16 only (the engine's fixed capture format).
 */
class NoiseSuppressor {
public:
  static constexpr int kSampleRate = 48000;
  static constexpr size_t kFftSize = 512;
  static constexpr size_t kHop = kFftSize / 2;
  static constexpr size_t kBins = kFftSize / 2 + 1;
  // Prefill (kHop zeros) + one hop of overlap-add reconstruction = kFftSize samples
  // (512 = 10.7 ms), not kHop: the first output hop only becomes fully reconstructed
  // once the analysis window has slid a further hop past the prefill.
  static constexpr size_t kLatencySamples = kFftSize;

  explicit NoiseSuppressor(int sample_rate = kSampleRate);

  /** In place. Any length. Output is the input delayed by kLatencySamples, denoised. */
  void Process(int16_t* pcm, size_t samples);
  void Reset();
  /** Mean noise-floor estimate across bins, dBFS (negative). -100 before the first frame. */
  float last_noise_floor_dbfs() const { return noise_floor_dbfs_; }

private:
  void ProcessHop();
  static void Fft(std::vector<std::complex<float>>& x, bool inverse);

  std::vector<float> window_;              // sqrt-Hann, kFftSize
  std::vector<float> in_fifo_;             // pending input samples (float, -1..1)
  std::vector<float> out_fifo_;            // processed samples ready to emit
  std::vector<float> analysis_;            // last kFftSize input samples
  std::vector<float> overlap_;             // kFftSize synthesis overlap-add tail
  std::vector<std::complex<float>> spec_;  // kFftSize
  std::vector<float> noise_psd_;           // kBins
  std::vector<float> smoothed_psd_;        // kBins
  std::vector<float> prev_gain_;           // kBins
  std::vector<float> prev_mag2_;           // kBins, |X|^2 of previous frame
  std::vector<float> gain_;                // kBins
  uint32_t frames_ = 0;
  float noise_floor_dbfs_ = -100.f;
};

} // namespace pbr
