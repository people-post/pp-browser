#pragma once

#include "feature/ui/shell/ShellFeedbackPorts.h"

#include "common/Error.h"

#include <functional>
#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** Thin shell feedback helpers for end-user-visible outcomes after actions. */
struct UserFeedback {
  /** Resolve display text via AppError::Display. */
  static std::string UserMessage(const Error& err);

  /** App fills toast/banner ports from ShellHost. Clear via BindPorts({}). */
  static void BindPorts(ShellFeedbackPorts ports);

  static void Ok(const std::string& message);
  static void Fail(const std::string& message);
  /** Toast AppError::Display; returns that string for status_ binding. */
  static std::string FailFrom(const Error& err);
  static void NeedsSetup(const std::string& message);
  static void Alert(const std::string& title, const std::string& message, std::function<void()> on_ok = {},
                    const std::string& ok_label = {});
  static void Confirm(const std::string& title, const std::string& message,
                      std::function<void(bool)> on_result, const std::string& ok_label = {});

private:
  static ShellFeedbackPorts ports_;
};

} // namespace pbr
