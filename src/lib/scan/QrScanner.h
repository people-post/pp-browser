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
  /** Why a scan failed, for choosing the hint without reading `error`'s text. */
  enum class ErrorKind { None, PermissionDenied, CameraBusy, Other };

  struct Update {
    std::optional<VideoFrameRgba> preview;
    std::optional<std::string> text;
    std::string error;
    ErrorKind error_kind = ErrorKind::None;
    /** The camera is one the call screen draws mirrored (not a back camera). A hint only: `preview` is never mirrored
     *  here, the page decides whether to draw it so. */
    bool mirror_preview = false;
  };

  /** `no_frame_timeout` is how long a camera may stay silent (outside an OS permission prompt) before an error. */
  explicit QrScanner(MediaDeviceArbiter& devices,
                     std::chrono::milliseconds no_frame_timeout = std::chrono::seconds(15));
  ~QrScanner();
  QrScanner(const QrScanner&) = delete;
  QrScanner& operator=(const QrScanner&) = delete;
  /** The OS lists a camera, so a scan could open one (nothing is opened to find out). */
  bool CameraAvailable() const;
  /**
   * Replaces any previous scan; rear camera preferred. Pass UI-thread display rotation. `full_frame_preview`: the
   * preview is always the whole frame (up to 1280 on its long edge) instead of the centre crop the decoder is
   * sometimes given while it hunts for a code; what is decoded does not change.
   */
  void Start(int display_rotation_deg = 0, bool full_frame_preview = false);
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
