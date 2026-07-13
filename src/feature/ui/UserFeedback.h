#pragma once

#include "common/Error.h"

#include <string>

namespace pbr {

/** Thin shell feedback helpers for end-user-visible outcomes after actions. */
struct UserFeedback {
  /** Resolve display text via AppError::Display. */
  static std::string UserMessage(const Error& err);

  static void Ok(const std::string& message);
  static void Fail(const std::string& message);
  /** Toast AppError::Display; returns that string for status_ binding. */
  static std::string FailFrom(const Error& err);
  static void NeedsSetup(const std::string& message);
};

} // namespace pbr
