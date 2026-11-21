#pragma once

#include "common/Error.h"
#include "common/ExtArea.h"
#include "common/Value.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "common/PbrCompat.h"

namespace pbr {

/**
 * The receiver's order (docs/contracts/SIGNED_EXTENSIONS.md § The receiver's order), the same for every signed
 * object, run by `ReceiveSigned` so each object only fills in its steps:
 *
 *   1. version     the outside version: missing or below the floor → Older; above what this build reads →
 *                  NeedsUpdate, nothing parsed or verified (never reported as a forgery)
 *   2. parse       that version's fields, the extension area included → Malformed
 *   3. algorithms  every typed blob's id (`ReadTypedBlob*`): unknown → NeedsUpdate; malformed → Malformed
 *   4. verify      the signature over the bytes rebuilt with the version read (`SignedBytesWriter`) → Forged
 *   5. extensions  an unknown critical entry → NeedsUpdate (after the signature: an unsigned object must not raise
 *                  "update needed"); unknown non-critical entries are ignored and kept
 *   6. semantics   freshness, bounds, who may sign: the caller's, after an Accepted receipt
 *
 * A step's error that is `AppError::NewerFormat` is NeedsUpdate wherever it comes from before step 4 (a typed blob's
 * unknown algorithm, a nested versioned field); any other error is that step's refusal.
 */
enum class SignedVerdict : uint8_t {
  Accepted = 0,
  /** Outside version missing or below the floor: made by an older app, outside the range. Refused. */
  Older = 1,
  /** A newer version, an unknown algorithm id or an unknown critical entry: update needed, not acted on. */
  NeedsUpdate = 2,
  /** Fields that do not parse: sizes, bounds, a bad extension area, a malformed typed blob. Refused. */
  Malformed = 3,
  /** The signature does not verify over the rebuilt bytes: a forgery. Hard refusal. */
  Forged = 4,
};

/** "accepted", "older", "needs_update", "malformed", "forged" (logs and vector files). */
std::string_view SignedVerdictName(SignedVerdict verdict);

/** The versions an object's receiver takes: release 1 accepts only its own (`floor == current`). */
struct SignedVersions {
  int64_t floor = 0;
  int64_t current = 0;
};

/** One object's steps for `ReceiveSigned`. The callbacks run in order, each only if the one before passed. */
struct SignedReceiveSteps {
  /** The object, for the log ("account card"). */
  std::string_view what;
  /** Step 1: the outside version read (`OutsideVersionOfJson` / `OutsideVersionOfBlob`); nullopt when missing. */
  std::optional<int64_t> outside_version;
  SignedVersions versions;
  /** Step 2: parse the fields of `*outside_version` (it is in range), the extension area into `*ext`. */
  std::function<Roe<void>()> parse;
  /** Step 3: read each typed blob (`ReadTypedBlob*`, `TypedRead::ToError`). May be empty when parse reads them. */
  std::function<Roe<void>()> algorithms;
  /** Step 4: rebuild the signed bytes and verify (`VerifyTyped`). */
  std::function<Roe<void>()> verify;
  /** Step 5: the area parse filled; null is the empty area. Read only after verify passed. */
  const pp::ExtArea* ext = nullptr;
  /** Critical types (whole u16, the critical bit set) this build understands in this object: none at release 1. */
  std::span<const uint16_t> understood_critical = {};
};

/** What `ReceiveSigned` decided. */
struct SignedReceipt {
  SignedVerdict verdict = SignedVerdict::Malformed;
  /** The outside version read (0 when missing). */
  int64_t version = 0;
  /** What failed, for the log: the object, the step, the reason (empty when Accepted). */
  std::string detail;

  bool accepted() const { return verdict == SignedVerdict::Accepted; }
  bool needs_update() const { return verdict == SignedVerdict::NeedsUpdate; }
  /**
   * NeedsUpdate → `AppError::NewerFormat(detail)` (the receiver says "update needed", ReceiveFailure::NeedsUpdate);
   * Older, Malformed and Forged → a plain refusal `Error(detail)`. Only meaningful when not accepted.
   */
  Error ToError() const;
  /** Success when accepted, else `ToError()`. */
  Roe<void> ToRoe() const;
};

/** Run steps 1–5 of the receiver's order for one object. */
SignedReceipt ReceiveSigned(const SignedReceiveSteps& steps);

/** A JSON object's outside version: the integer at `key`; nullopt when missing or not an integer. */
std::optional<int64_t> OutsideVersionOfJson(const pp::common::Object& object, const std::string& key);
/** A binary blob's outside version: its first byte; nullopt when empty. */
std::optional<int64_t> OutsideVersionOfBlob(std::span<const uint8_t> blob);

} // namespace pbr
