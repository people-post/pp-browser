#pragma once

#include "common/directory/KeyCard.h"
#include "domain/peers/AccountKeyResolver.h"
#include "lib/messaging/AccountCard.h"
#include "lib/people/IdentityStore.h"

#include "common/Error.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** What a device that its account's newest list leaves out is told (M021 d4). */
inline constexpr const char kSignedOutMessage[] =
    "This device was signed out of your account. Link it again from your main device to use it.";

/** The account's devices as this device knows them (M021 d4): the newest card's list; its first is the master. */
struct OwnDeviceList {
  std::vector<AccountCardDevice> devices;
  std::string self_peer_id;
  /** The newest list leaves this device out (and it is not joining or starting a fresh list). */
  bool signed_out = false;
};

/**
 * This account's own cards (docs/contracts/ACCOUNT_CARD.md): the key card, and the account card — the
 * account's device list (M021), kept with the other accounts' cards. The newest card wins (no union). Its first
 * device is the **master** (d4): only it changes the list — takes a device out, hands the role on. Any listed
 * device re-issues the card to bring its own entry or the nickname up to date, the list otherwise as it is; a
 * linked device appends itself once (`card_intent` join), a restored one starts a fresh list holding only itself
 * (`fresh`). A device the newest list leaves out is signed out: it issues nothing. Thread-safe.
 */
class OwnCards {
public:
  OwnCards(IdentityStore& identity, AccountKeyResolver& cards) : identity_(identity), cards_(cards) {}

  Roe<KeyCard> Keys();
  /**
   * The account card: the newest this device knows, re-issued when this device's entry or the nickname changed
   * (or to join / start fresh, per `card_intent`). An error (kSignedOutMessage) when this device is signed out.
   */
  Roe<AccountCard> Card(int64_t now_ms);
  /**
   * A card of our own account from a sibling (the link bundle, a sibling's notice): checked and kept; the newest
   * wins. The nickname is the account's: the newest card's becomes this device's too. Not an error when it leaves
   * this device out (see SignedOut).
   */
  Roe<AccountCard> Learn(const AccountCard& card, int64_t now_ms);
  /** The account's devices as this device knows them. */
  Roe<OwnDeviceList> Devices();
  /** The newest list leaves this device out: it stops sending (M021 d4). */
  bool SignedOut();
  /** The master takes another device off the list (revoke, M021 d4); an error on any other device. */
  Roe<AccountCard> RemoveDevice(const std::string& peer_id, int64_t now_ms);
  /** The master hands its role to another listed device (moves it first). */
  Roe<AccountCard> MakeMaster(const std::string& peer_id, int64_t now_ms);
  /**
   * The master gave out a transfer code: until `until_ms` a device that an older card of ours lists and the newest
   * does not is taken as joining (appended), not as signed out — the new device's card may reach the master after
   * the master issued another (a rename). In memory: a restart, or a restored master, expects none.
   */
  void ExpectJoin(int64_t until_ms);
  /** This device's entry on the card. */
  Roe<AccountCardDevice> Self();
  /** This device's PeerId (its mesh identity, from the device key). */
  Roe<std::string> DevicePeerId();
  /**
   * Runs when the account's card changed here — this device issued one, or took a sibling's newer one — so its
   * siblings hear of it and its own devices are reached; outside any lock.
   */
  void SetOnChanged(std::function<void(const AccountCard&)> on_changed) { on_changed_ = std::move(on_changed); }
  /**
   * Runs on the master when a card it takes in lists a device it did not (one just linked): the account's contacts
   * are worth telling (M021 d3) — the master has them; the new device may not yet. Outside any lock.
   */
  void SetOnDeviceJoined(std::function<void(const AccountCard&)> on_joined) { on_joined_ = std::move(on_joined); }

private:
  /** The card listing `devices`: `base` when it says it all, else a new one (`issued` set). */
  Roe<AccountCard> IssueLocked(const std::optional<AccountCard>& base, std::vector<AccountCardDevice> devices,
                               const std::string& nickname, int64_t now_ms, bool& issued);
  Roe<AccountCard> ChangeList(const std::string& peer_id, bool make_master, int64_t now_ms);
  Roe<std::optional<AccountCard>> NewestLocked();

  IdentityStore& identity_;
  AccountKeyResolver& cards_;
  std::mutex mutex_;  // issuing a card
  std::mutex keys_mutex_;
  std::optional<KeyCard> keys_;
  std::function<void(const AccountCard&)> on_changed_;
  std::function<void(const AccountCard&)> on_joined_;
  int64_t join_expected_until_ms_ = 0;  // under mutex_
};

} // namespace pbr
