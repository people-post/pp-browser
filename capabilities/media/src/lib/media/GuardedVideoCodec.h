#pragma once

#include "lib/media/IVideoCodec.h"

#include <chrono>
#include <functional>
#include <memory>

namespace pbr {

/** What a codec is for, in logs and `media.codec_stuck`. */
enum class VideoCodecRole : uint8_t { Encoder, Decoder };

/**
 * How long a hardware codec call may take (media-client-layers L016). An encode or decode is a frame:
 * milliseconds normally, so a second is already a wedge.
 */
struct VideoCodecBudgets {
  std::chrono::milliseconds create{5000};
  std::chrono::milliseconds configure{3000};
  std::chrono::milliseconds frame{1500};
  std::chrono::milliseconds reset{3000};
};

/**
 * A hardware codec (VideoToolbox, MediaCodec, VA-API, Media Foundation) made by `make` and used on a
 * thread of its own: every call waits at most its budget. A call past it — the vendor codec wedged —
 * leaves the codec NotResponding: later calls fail at once, the holder drops it and makes a new one.
 * The dropped codec is destroyed on its own thread whenever the stuck call returns (or never), so
 * neither its holder nor whoever drops it ever waits on it.
 */
std::unique_ptr<IVideoCodec> MakeGuardedVideoCodec(std::function<std::unique_ptr<IVideoCodec>()> make,
                                                   VideoCodecRole role, VideoCodecBudgets budgets = {});

} // namespace pbr
