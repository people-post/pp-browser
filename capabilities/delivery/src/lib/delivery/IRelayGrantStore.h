#pragma once

#include "common/Error.h"
#include "common/chat/RelayGrant.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {

/**
 * The relay grants of this account (multi-relay MR005): the ones others gave us, attached to our sends into their
 * inboxes, and when we gave each of ours, to renew it in time. Any thread. The app's storage keeps them in
 * profile.db (CR024); tests use an in-memory one.
 */
class IRelayGrantStore {
public:
  /** A grant this account gave, and whether its grantee confirmed it kept it (`relay_grant_ack`). */
  struct Issued {
    RelayGrant grant;
    bool confirmed = false;
  };

  virtual ~IRelayGrantStore() = default;

  /** The grant `recipient_account_id` gave us; none when we hold none. */
  virtual Roe<std::optional<RelayGrant>> Received(const std::string& recipient_account_id) = 0;
  /** Keep `grant` (one given to us): over the one held for its recipient only when issued later. */
  virtual Roe<void> PutReceived(const RelayGrant& grant) = 0;
  virtual Roe<void> DropReceived(const std::string& recipient_account_id) = 0;

  virtual Roe<std::optional<Issued>> IssuedTo(const std::string& sender_account_id) = 0;
  /** Keep `issued` as the grant given to its sender (replacing the one held). */
  virtual Roe<void> PutIssued(const Issued& issued) = 0;
  virtual Roe<void> DropIssued(const std::string& sender_account_id) = 0;
};

} // namespace pbr
