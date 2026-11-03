#pragma once

#include "lib/media/IVideoCodec.h"
#include "lib/media/MediaDeviceArbiter.h"

#include <memory>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The lab's stand-in for a camera and a hardware H.264 codec (pp-app-probe --synthetic-video): no GPU and no
 * camera in a container, and the question there is the transport's, not the picture's. Access units are sized
 * like the real encoder's at the same settings — a keyframe of kKeyframeBytes every two seconds (or when asked),
 * the rest sized to the target bitrate — so the network carries what a phone sends; each decodes to a small
 * grey frame.
 */
struct SyntheticVideo {
  /** A 640×360 keyframe of the Constrained Baseline encoder at about 400 kbit/s. */
  static constexpr size_t kKeyframeBytes = 20'000;
  /** Before the engine sets a target (the call profile's 400 kbit/s). */
  static constexpr int64_t kDefaultTargetBps = 400'000;

  static std::unique_ptr<IVideoCodec> CreateCodec();
  /** `inner`'s audio, and a camera that has a frame whenever asked. */
  static std::unique_ptr<IMediaDeviceBackend> WithCamera(std::unique_ptr<IMediaDeviceBackend> inner);
};

} // namespace pbr
