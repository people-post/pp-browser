#pragma once

#include "common/Error.h"
#include "common/Value.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr::relay {

/**
 * The message relay's settings (projects/node-relay NR007): the node config's `node.relay` section, each key with a
 * `PP_NODE_RELAY_<KEY>` override (environment over file). Tier-free: the same keys whichever build has the relay.
 * Off until enabled (Hongwei, 2026-11-27): a node never keeps mail for accounts unasked.
 */
struct RelaySettings {
  /** `enabled` / PP_NODE_RELAY_ENABLED. */
  bool enabled = false;
  /** `retention_days` / PP_NODE_RELAY_RETENTION_DAYS: how long a record is kept at most (a sender may ask shorter). */
  int64_t retention_days = 7;
  /** `max_records` / PP_NODE_RELAY_MAX_RECORDS: records waiting per account. */
  int64_t max_records = 1000;
  /** `max_bytes` / PP_NODE_RELAY_MAX_BYTES: bytes waiting per account (the sealed records' sizes). */
  int64_t max_bytes = 16 * 1024 * 1024;

  static constexpr int64_t kMaxRetentionDays = 30; // the HTTPS relay's longest
  static constexpr int64_t kMaxRecordsCap = 1'000'000;
  static constexpr int64_t kMinBytes = 256 * 1024; // one sealed record of the largest size (128 KiB) and room
  static constexpr int64_t kMaxBytesCap = int64_t{64} * 1024 * 1024 * 1024;

  /** Environment lookup: nullptr when unset. */
  using Env = std::function<const char*(const char*)>;

  /**
   * Reads `section` (null: none written) and the environment. A value of the wrong type or outside its bounds is an
   * error naming the key, so a typo never quietly keeps the default; keys it does not know go to `unknown`.
   */
  static Roe<RelaySettings> Read(const Object* section, const Env& env, std::vector<std::string>* unknown = nullptr);

  /** "on, keeps 7 days, 1000 records / 16 MiB per account" (for the start log). */
  std::string Describe() const;
};

} // namespace pbr::relay
