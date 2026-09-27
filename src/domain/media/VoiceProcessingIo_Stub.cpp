#include "domain/media/VoiceProcessingIo.h"

namespace pbr {

struct VoiceProcessingIo::Impl {};

VoiceProcessingIo::VoiceProcessingIo() : impl_(std::make_unique<Impl>()) {}
VoiceProcessingIo::~VoiceProcessingIo() = default;

bool VoiceProcessingIo::Open(std::string* reason) {
  if (reason) {
    *reason = "unsupported";
  }
  return false;
}
void VoiceProcessingIo::Close() {}
bool VoiceProcessingIo::IsOpen() const { return false; }
size_t VoiceProcessingIo::ReadCapture(int16_t*, size_t) { return 0; }
size_t VoiceProcessingIo::WritePlayout(const int16_t*, size_t) { return 0; }
size_t VoiceProcessingIo::QueuedPlayoutBytes() const { return 0; }
bool VoiceProcessingIo::TakeDeviceChanged() { return false; }
uint64_t VoiceProcessingIo::PlayoutUnderruns() const { return 0; }
size_t VoiceProcessingIo::RenderChunkBytes() const { return 0; }
std::string VoiceProcessingIo::TakeDiag() { return {}; }

}  // namespace pbr
