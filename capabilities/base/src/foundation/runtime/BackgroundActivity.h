#pragma once

#include <memory>
#include <string>

namespace pbr {

/**
 * Asks the OS for time to finish a transfer if the app goes to the background meanwhile: iOS grants about 30 s
 * (`beginBackgroundTaskWithExpirationHandler:`); elsewhere it does nothing (desktops do not suspend a running
 * app; Android resumes an unfinished send at the next foreground). Hold one for the whole of an upload or a
 * download; it ends when it goes out of scope. When the granted time runs out the OS handler only logs and ends
 * the task: the transfer's own failure path deals with being cut off. Backends: BackgroundActivity_{Ios,Default}.
 */
class BackgroundActivity {
public:
  /** `name` shows in the OS's diagnostics and the log; no user content. */
  explicit BackgroundActivity(const std::string& name);
  ~BackgroundActivity();
  BackgroundActivity(const BackgroundActivity&) = delete;
  BackgroundActivity& operator=(const BackgroundActivity&) = delete;

  /** Per-backend state (the iOS task id); opaque here. */
  struct State;

private:
  std::shared_ptr<State> state_;
};

} // namespace pbr
