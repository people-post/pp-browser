#include "domain/media/NoiseSuppressor.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace pbr {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kPsdSmooth = 0.8f;      // power smoothing for the SNR gate
constexpr float kNoiseAdapt = 0.05f;    // floor adaption when frame classed as noise
constexpr float kNoiseCreep = 1.0005f;  // slow upward creep during speech (never lock up)
constexpr float kNoiseGateSnr = 3.0f;   // a-posteriori SNR below which a bin is "noise"
constexpr float kAprioriBeta = 0.98f;   // decision-directed smoothing
constexpr float kGainFloor = 0.125f;    // -18 dB
constexpr uint32_t kInitFrames = 40;    // ~0.2 s at 256-sample hops: first frames initialise the floor
constexpr float kEps = 1e-12f;

} // namespace

NoiseSuppressor::NoiseSuppressor(int sample_rate) {
  if (sample_rate != kSampleRate) {
    throw std::invalid_argument("NoiseSuppressor supports 48 kHz only");
  }
  window_.resize(kFftSize);
  for (size_t n = 0; n < kFftSize; ++n) {
    // Periodic Hann → sqrt for WOLA; hann(n)+hann(n+hop) == 1 so sqrt·sqrt overlap-adds to 1.
    const float hann = 0.5f - 0.5f * std::cos(2.f * kPi * static_cast<float>(n) / static_cast<float>(kFftSize));
    window_[n] = std::sqrt(hann);
  }
  Reset();
}

void NoiseSuppressor::Reset() {
  in_fifo_.clear();
  out_fifo_.assign(kHop, 0.f); // prefill: emit kHop zeros first
  analysis_.assign(kFftSize, 0.f);
  overlap_.assign(kFftSize, 0.f);
  spec_.assign(kFftSize, {});
  noise_psd_.assign(kBins, 0.f);
  smoothed_psd_.assign(kBins, 0.f);
  prev_gain_.assign(kBins, 1.f);
  prev_mag2_.assign(kBins, 0.f);
  gain_.assign(kBins, 1.f);
  frames_ = 0;
  noise_floor_dbfs_ = -100.f;
}

void NoiseSuppressor::Process(int16_t* pcm, size_t samples) {
  in_fifo_.reserve(in_fifo_.size() + samples);
  for (size_t i = 0; i < samples; ++i) {
    in_fifo_.push_back(static_cast<float>(pcm[i]) / 32768.f);
  }
  while (in_fifo_.size() >= kHop) {
    ProcessHop();
  }
  // out_fifo_ always holds >= samples here: it starts with kHop zeros and every hop adds kHop.
  for (size_t i = 0; i < samples; ++i) {
    const float v = std::clamp(out_fifo_[i] * 32768.f, -32768.f, 32767.f);
    pcm[i] = static_cast<int16_t>(std::lround(v));
  }
  out_fifo_.erase(out_fifo_.begin(), out_fifo_.begin() + static_cast<std::ptrdiff_t>(samples));
}

void NoiseSuppressor::ProcessHop() {
  // Slide analysis buffer by one hop and append the new samples.
  std::copy(analysis_.begin() + kHop, analysis_.end(), analysis_.begin());
  std::copy(in_fifo_.begin(), in_fifo_.begin() + kHop, analysis_.begin() + kHop);
  in_fifo_.erase(in_fifo_.begin(), in_fifo_.begin() + kHop);

  for (size_t n = 0; n < kFftSize; ++n) {
    spec_[n] = {analysis_[n] * window_[n], 0.f};
  }
  Fft(spec_, false);

  ++frames_;
  float floor_acc = 0.f;
  for (size_t k = 0; k < kBins; ++k) {
    const float mag2 = std::norm(spec_[k]);
    smoothed_psd_[k] = kPsdSmooth * smoothed_psd_[k] + (1.f - kPsdSmooth) * mag2;
    if (frames_ <= kInitFrames) {
      // Initialise floor from the first frames (assumed mostly noise / silence).
      noise_psd_[k] = frames_ == 1 ? mag2 : 0.9f * noise_psd_[k] + 0.1f * mag2;
    } else {
      const float post = smoothed_psd_[k] / (noise_psd_[k] + kEps);
      if (post < kNoiseGateSnr) {
        noise_psd_[k] = (1.f - kNoiseAdapt) * noise_psd_[k] + kNoiseAdapt * smoothed_psd_[k];
      } else {
        noise_psd_[k] *= kNoiseCreep;
      }
      noise_psd_[k] = std::min(noise_psd_[k], smoothed_psd_[k] + kEps);
    }
    floor_acc += noise_psd_[k];

    // Decision-directed a-priori SNR (Ephraim-Malah) and Wiener gain.
    const float gamma = mag2 / (noise_psd_[k] + kEps);
    const float xi_prev = prev_gain_[k] * prev_gain_[k] * prev_mag2_[k] / (noise_psd_[k] + kEps);
    const float xi = kAprioriBeta * xi_prev + (1.f - kAprioriBeta) * std::max(gamma - 1.f, 0.f);
    float g = xi / (1.f + xi);
    if (frames_ <= kInitFrames) {
      g = 1.f;
    }
    gain_[k] = std::max(g, kGainFloor);
    prev_mag2_[k] = mag2;
  }
  // Mean per-sample noise variance in dBFS. For a sqrt-Hann window, Σw² = kFftSize/2, so an
  // unwindowed white-noise variance σ² comes out of the FFT as a per-bin PSD of σ²·(kFftSize/2)
  // on average; dividing by kFftSize/2 undoes that and 10·log10(σ²) is the correct dBFS-power.
  {
    const float norm = static_cast<float>(kFftSize / 2);
    const float mean = floor_acc / static_cast<float>(kBins) / norm;
    noise_floor_dbfs_ = 10.f * std::log10(std::max(mean, 1e-10f));
  }
  // 3-tap smoothing across bins (reduces musical noise).
  for (size_t k = 1; k + 1 < kBins; ++k) {
    prev_gain_[k] = 0.25f * gain_[k - 1] + 0.5f * gain_[k] + 0.25f * gain_[k + 1];
  }
  prev_gain_[0] = gain_[0];
  prev_gain_[kBins - 1] = gain_[kBins - 1];

  // Apply gain (Hermitian symmetric).
  for (size_t k = 0; k < kBins; ++k) {
    spec_[k] *= prev_gain_[k];
  }
  for (size_t k = 1; k + 1 < kBins; ++k) {
    spec_[kFftSize - k] = std::conj(spec_[k]);
  }
  Fft(spec_, true);

  // Synthesis window + overlap-add; emit one hop.
  for (size_t n = 0; n < kFftSize; ++n) {
    overlap_[n] += spec_[n].real() * window_[n];
  }
  out_fifo_.insert(out_fifo_.end(), overlap_.begin(), overlap_.begin() + kHop);
  std::copy(overlap_.begin() + kHop, overlap_.end(), overlap_.begin());
  std::fill(overlap_.begin() + kHop, overlap_.end(), 0.f);
}

// Iterative radix-2 complex FFT (kFftSize is a power of two). Inverse includes the 1/N scale.
void NoiseSuppressor::Fft(std::vector<std::complex<float>>& x, bool inverse) {
  const size_t n = x.size();
  for (size_t i = 1, j = 0; i < n; ++i) {
    size_t bit = n >> 1;
    for (; j & bit; bit >>= 1) {
      j ^= bit;
    }
    j ^= bit;
    if (i < j) {
      std::swap(x[i], x[j]);
    }
  }
  for (size_t len = 2; len <= n; len <<= 1) {
    const float ang = 2.f * kPi / static_cast<float>(len) * (inverse ? 1.f : -1.f);
    const std::complex<float> wlen(std::cos(ang), std::sin(ang));
    for (size_t i = 0; i < n; i += len) {
      std::complex<float> w(1.f, 0.f);
      for (size_t j = 0; j < len / 2; ++j) {
        const std::complex<float> u = x[i + j];
        const std::complex<float> v = x[i + j + len / 2] * w;
        x[i + j] = u + v;
        x[i + j + len / 2] = u - v;
        w *= wlen;
      }
    }
  }
  if (inverse) {
    const float inv = 1.f / static_cast<float>(n);
    for (auto& v : x) {
      v *= inv;
    }
  }
}

} // namespace pbr
