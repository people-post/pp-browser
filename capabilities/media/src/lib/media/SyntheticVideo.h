#pragma once

#include "lib/media/IVideoCodec.h"
#include "lib/media/MediaDeviceArbiter.h"

#include <memory>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The lab's stand-in for a camera and a hardware H.264 codec (pp-app-probe --synthetic-video): no GPU and no
 * camera in a container, and the question there is the transport's, not the picture's. Access units are sized
 * like the real encoder's at the same settings — a keyframe of kKeyframeSeconds of the target every
 * kKeyframeIntervalSeconds (or when asked), the rest sized to the target bitrate — so the network carries what a
 * phone sends; each decodes to a small grey frame.
 */
struct SyntheticVideo {
  /** A keyframe carries about this long of the target bitrate (20 KB at 400 kbit/s, the hardware encoders'). */
  static constexpr double kKeyframeSeconds = 0.4;
  /** Before the engine sets a target (the call profile's). */
  static constexpr int64_t kDefaultTargetBps = 250'000;

  static std::unique_ptr<IVideoCodec> CreateCodec();
  /** `inner`'s audio, and a camera that has a frame whenever asked. */
  static std::unique_ptr<IMediaDeviceBackend> WithCamera(std::unique_ptr<IMediaDeviceBackend> inner);
};

} // namespace pbr
