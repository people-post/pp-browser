#pragma once

#include "gui/shell/ShellFeedbackPorts.h"

#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Thin shell feedback helpers for end-user-visible outcomes after actions. */
struct UserFeedback {
  /** Resolve display text via AppError::Display. */
  static std::string UserMessage(const Error& err);
  /**
   * `UserMessage`, except that while the device is offline a failure that never got an answer from the server
   * (a Network error that is not an HTTP response) says so plainly instead of naming a guessed cause.
   */
  static std::string UserMessage(const Error& err, bool device_offline);
  /**
   * An attachment send that failed. `offline_stop`: the app itself stopped the transfer because the device had been
   * offline for a while (its progress answered "stop", which the HTTP layer reports as a cancelled request): "No
   * network connection…". Otherwise `UserMessage(err, device_offline)` — only network-shaped failures say "offline";
   * local ones (storage, shutting down) and errors with their own words keep them.
   */
  static std::string AttachmentFailureMessage(const Error& err, bool device_offline, bool offline_stop);

  /** App fills toast/banner ports from ShellHost. Clear via BindPorts({}). */
  static void BindPorts(ShellFeedbackPorts ports);

  static void Ok(const std::string& message);
  static void Fail(const std::string& message);
  /** A note the user should have time to read (the long pill, as failures get): not a result, not an error. */
  static void Note(const std::string& message);
  /** Takes the bottom pill down at once (for a Note that said "Preparing…" and whose work is done). */
  static void DismissToast();
  /** Toast AppError::Display; returns that string for status_ binding. */
  static std::string FailFrom(const Error& err);
  /** Banner. Gone after 5 s unless `sticky` (something the user must act on). */
  static void NeedsSetup(const std::string& message, bool sticky = false);
  static void Alert(const std::string& title, const std::string& message, std::function<void()> on_ok = {},
                    const std::string& ok_label = {});
  static void Confirm(const std::string& title, const std::string& message, std::function<void(bool)> on_result,
                      const std::string& ok_label = {});
  /** Confirm whose OK ends / removes something: the OK label in the danger colour on the phone. */
  static void ConfirmDestructive(const std::string& title, const std::string& message,
                                 std::function<void(bool)> on_result, const std::string& ok_label = {});

private:
  static ShellFeedbackPorts ports_;
};

} // namespace pbr
