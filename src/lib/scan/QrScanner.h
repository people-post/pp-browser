#pragma once

#include "lib/media/MediaDeviceArbiter.h"
#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace pbr {
/** One local camera scan at a time. Start/Stop/Poll are nonblocking, called by its owner thread. */
class QrScanner {
public:
  struct Update {
    std::optional<VideoFrameRgba> preview;
    std::optional<std::string> text;
    std::string error;
  };

  /** `no_frame_timeout` is how long a camera may stay silent (outside an OS permission prompt) before an error. */
  explicit QrScanner(MediaDeviceArbiter& devices,
                     std::chrono::milliseconds no_frame_timeout = std::chrono::seconds(15));
  ~QrScanner();
  QrScanner(const QrScanner&) = delete;
  QrScanner& operator=(const QrScanner&) = delete;
  /** Replaces any previous scan; rear camera preferred. Pass UI-thread display rotation. */
  void Start(int display_rotation_deg = 0);
  /** Cancels, clears pending updates, never waits on OS permission or camera close. */
  void Stop();
  /** Consumes latest preview / terminal payload or error. A payload stops capture. */
  Update Poll();
  /** Stop and join before arbiter shutdown / SDL_Quit; destructor also joins. */
  void Shutdown();

private:
  struct State;
  std::unique_ptr<State> state_;
};
} // namespace pbr
