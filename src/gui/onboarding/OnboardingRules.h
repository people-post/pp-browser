#pragma once

#include <string>

namespace pbr {

/** First-run wizard steps after the PIN gate. Pure rules (no RmlUi, no storage). */
enum class OnboardingStep { None, Name, Account, Friends };

struct OnboardingFacts {
  /** Vault unlocked and the identity loaded. */
  bool identity_ready = false;
  /** Registered on the network (account set up). */
  bool registered = false;
  /** The wizard was finished or skipped before. */
  bool completed = false;
};

/**
 * Where the wizard starts, or None when it should not show: never before the identity is ready, never
 * again once finished or skipped; a registered account jumps past the name step.
 */
inline OnboardingStep OnboardingStartStep(const OnboardingFacts& facts) {
  if (!facts.identity_ready || facts.completed) {
    return OnboardingStep::None;
  }
  return facts.registered ? OnboardingStep::Account : OnboardingStep::Name;
}

inline const char* OnboardingStepKey(const OnboardingStep step) {
  switch (step) {
    case OnboardingStep::Name:
      return "name";
    case OnboardingStep::Account:
      return "account";
    case OnboardingStep::Friends:
      return "friends";
    case OnboardingStep::None:
      break;
  }
  return "";
}

/**
 * What the name field starts with: a fresh identity carries the placeholder nickname "user" (IdentityStore), which must
 * not look like a typed name (everyone would stay "user"); the field starts empty so its grey hint shows.
 */
inline std::string OnboardingNameFieldStart(const std::string& stored_nickname) {
  return stored_nickname == "user" ? std::string() : stored_nickname;
}

/** Nothing but whitespace (or nothing) typed: the Register button stays off. */
inline bool OnboardingNameIsBlank(const std::string& typed) {
  return typed.find_first_not_of(" \t\r\n") == std::string::npos;
}

/** The next step after `step`; Friends is the last one. */
inline OnboardingStep OnboardingNextStep(const OnboardingStep step) {
  switch (step) {
    case OnboardingStep::Name:
      return OnboardingStep::Account;
    case OnboardingStep::Account:
      return OnboardingStep::Friends;
    case OnboardingStep::Friends:
    case OnboardingStep::None:
      break;
  }
  return OnboardingStep::None;
}

} // namespace pbr
