#pragma once

#include "common/Error.h"
#include "common/thread/IThreadStore.h"
#include "foundation/crypto/CryptoTypes.h"
#include "lib/calls/ICallSessionStore.h"
#include "lib/content/CasStore.h"
#include "lib/messaging/IGroupRosterStore.h"
#include "lib/messaging/IThreadMentionsStore.h"
#include "lib/people/ContactsStore.h"
#include "mesh/types/DirectoryTypes.h"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

class ConversationsHub;

/**
 * Fixed fake data for the UI demo app (kUiDemoBuild, DEV/PLAN-ui-demo-data.md): 12 contacts, 10 direct chats and one
 * unread chat with someone who is not a contact, 2 groups and one AI chat, so every screen can be compared with the design with long names, long messages, unread counts,
 * a quote reply and two pictures on show. Always compiled and covered by the normal build's tests; only the app's
 * ready callback calls it, and only when kUiDemoBuild.
 *
 * Fixed ids and upserts: each start puts back the same data. Nothing is ever written as Pending (no outbox row); the
 * contacts carry no relay user id, peer id or address, and the direct chats no peer identity, so a send is refused
 * before it is stored.
 */

/** Today 00:00 local time, in unix ms, for the day `now_ms` falls on. */
int64_t LocalMidnightMs(int64_t now_ms);

struct DemoSeedDeps {
  IThreadStore& threads;
  ContactsStore& contacts;
  IGroupRosterStore& groups;
  CasStore& content;
  /** The profile directory (the pictures' views live under its threads/ folder). */
  std::string profile_dir;
  /** The attachment key of the unlocked profile (AttachmentFetchWorkflow::CopyDek). */
  ByteVector attachment_dek;
  uint64_t max_receive_bytes = 0;
  /** A non-empty relay user id means a real account: the seeder then writes nothing. */
  std::string own_relay_user_id;
  /** Every message time is a fixed offset from this (today 00:00 local). */
  int64_t today_midnight_ms = 0;
  /** `remote.fetched_at` of the contacts. */
  int64_t now_ms = 0;
  /** Reads a bundled asset ("demo/demo_landscape.png"); ReadDemoAsset in the app. */
  std::function<Roe<ByteVector>(const std::string& relative)> read_asset;
  /**
   * Gives the non-contact who wrote first a directory name, as the directory would (the demo has none); kept in
   * memory only. Unset: the chat shows the bare id.
   */
  std::function<void(const mesh::DirectoryHit&)> put_directory_hit;
  /** Home's Mentions widget: the unread-mention marker of the group line that mentions everyone. Unset: none. */
  const IThreadMentionsStore* mentions = nullptr;
  /** Home's Calls widget: two missed calls and one we made today go here (profile.db). Unset: no calls. */
  const ICallSessionStore* calls = nullptr;
  /** This account's id (its own row in those calls); empty: no calls written. */
  std::string local_account_id;
  /** Home's Favorites: stars these contacts when none is starred yet (the profile preferences). Unset: none. */
  std::function<void(const std::vector<std::string>& contact_ids)> star_contacts;
};

/** Reads `relative` through IAssetLocator (the app bundle on iOS, the APK on Android). */
Roe<ByteVector> ReadDemoAsset(const std::string& relative);

/** The contacts the demo stars for Home's Favorites (DemoSeedDeps::star_contacts). */
std::vector<std::string> DemoStarredContacts();

class DemoDataSeeder {
public:
  /**
   * Seeds once per process: later calls (the ready callback fires again after a re-unlock) write nothing and return
   * false. Returns true when it wrote the data; false when it skipped (already seeded, or the account is real).
   * An error leaves the once flag unset.
   */
  Roe<bool> SeedOnce(const DemoSeedDeps& deps);

private:
  bool done_ = false;
};

/** The deps of the running app, read from the hub's stores (call on the UI thread once messaging is ready). */
Roe<DemoSeedDeps> MakeDemoSeedDeps(ConversationsHub& hub);

/**
 * The demo app has no AI backend: the ask-AI panel streams this made-up Markdown answer instead, so the screens can
 * be seen (DEV/PLAN-ui-batch5-ask-ai.md 5a). A question containing "出错" or "error" gets "" (the turn fails), to show
 * the panel's error state. Chinese when the question has CJK text, else English.
 */
std::string DemoAskAiAnswer(const std::string& question);

/** A made-up headline for Home's Today's briefs widget in the demo app (it has no network). */
struct DemoHeadline {
  const char* title;
  const char* url;
  /** Published this many minutes before now. */
  int minutes_ago;
};
std::vector<DemoHeadline> DemoBriefHeadlines(bool chinese);

/**
 * The demo app has no peers: a message sent in a direct or group chat gets this short reply from the other side, so
 * the chat shows my message on the right and an answer on the left. Chinese when the message has CJK text.
 */
std::string DemoPeerReply(const std::string& sent);

} // namespace pbr
