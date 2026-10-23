#pragma once

#include <cstdint>
#include <vector>

namespace pbr {

/**
 * Synthesizes one 5.0 s cycle of the caller ringback tone (CN "嘟——嘟——"):
 * 1.0 s of 450 Hz sine at peak -12 dBFS (0.25 * INT16_MAX) with 10 ms
 * raised-cosine fade-in/out on the burst, followed by 4.0 s of silence.
 * Mono, 16-bit signed PCM at `sample_rate`.
 */
std::vector<int16_t> MakeRingbackCycle(int sample_rate);

} // namespace pbr
