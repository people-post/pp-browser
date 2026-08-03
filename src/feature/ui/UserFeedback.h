#pragma once

#include "feature/ui/ShellFeedbackPorts.h"

#include "common/Error.h"

#include <string>

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

private:
  static ShellFeedbackPorts ports_;
};

} // namespace pbr
