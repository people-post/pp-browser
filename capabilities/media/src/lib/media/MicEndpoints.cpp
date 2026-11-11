#include "lib/media/MicEndpoints.h"

#include <atomic>

namespace pbr {
namespace {

std::atomic<int> g_open_mics{0};

} // namespace

bool AnyMicEndpointOpen() {
  return g_open_mics.load(std::memory_order_acquire) > 0;
}

MicEndpointCount::MicEndpointCount() {
  g_open_mics.fetch_add(1, std::memory_order_acq_rel);
}

MicEndpointCount::~MicEndpointCount() {
  g_open_mics.fetch_sub(1, std::memory_order_acq_rel);
}

} // namespace pbr
