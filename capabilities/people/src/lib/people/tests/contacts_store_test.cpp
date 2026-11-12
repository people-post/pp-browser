#include "lib/people/ContactsStore.h"
#include "lib/people/ContactTypes.h"
#include "lib/people/ContactIdentity.h"
#include "lib/people/ContactJson.h"
#include "common/directory/DirectoryJson.h"
#include "lib/people/SupportContact.h"
#include "common/PlatformLimits.h"
#include "common/ValueJson.h"

#include <filesystem>
#include <fstream>
#include <optional>
#include <gtest/gtest.h>
#include <sstream>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace {

using namespace pbr;

class ContactsStoreTest : public ::testing::Test {
protected:
  void SetUp() override {
    data_dir_ = std::filesystem::temp_directory_path() /
                ("pp_browser_contacts_store_" + std::to_string(::testing::UnitTest::GetInstance()->random_seed()) +
                 "_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(data_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(data_dir_); }

  std::filesystem::path data_dir_;
};

TEST_F(ContactsStoreTest, RemoveDeletesContactAndPersists) {
  ContactsStore store(data_dir_.string());

  Contact contact;
  contact.id = "contact-alice";
  contact.display_name = "Alice";
  contact.server_nickname = "alice";
  contact.ids = {{mesh::ContactIdKind::RelayUser, "relay:alice123", true}};
  contact.trust = TrustLevel::Unknown;

  ASSERT_TRUE(static_cast<bool>(store.Upsert(contact))) << "upsert failed";

  auto listed = store.List();
  ASSERT_TRUE(static_cast<bool>(listed));
  ASSERT_EQ(listed->size(), 1u);

  auto removed = store.Remove(contact.id);
  ASSERT_TRUE(static_cast<bool>(removed)) << removed.error().message;
  EXPECT_TRUE(*removed);

  auto missing = store.Get(contact.id);
  ASSERT_TRUE(static_cast<bool>(missing));
  EXPECT_FALSE(missing->has_value());

  auto empty_list = store.List();
  ASSERT_TRUE(static_cast<bool>(empty_list));
  EXPECT_TRUE(empty_list->empty());

  ContactsStore reloaded(data_dir_.string());
  auto reloaded_get = reloaded.Get(contact.id);
  ASSERT_TRUE(static_cast<bool>(reloaded_get));
  EXPECT_FALSE(reloaded_get->has_value());
}

TEST_F(ContactsStoreTest, RemoveMissingReturnsFalse) {
  ContactsStore store(data_dir_.string());
  auto removed = store.Remove("does-not-exist");
  ASSERT_TRUE(static_cast<bool>(removed)) << removed.error().message;
  EXPECT_FALSE(*removed);
}

// projects/privacy: who may learn our address follows the address book as it changes.
TEST_F(ContactsStoreTest, ChangesNotifyOnceEach) {
  ContactsStore store(data_dir_.string());
  int changes = 0;
  store.SetOnChanged([&changes] { ++changes; });

  auto created = store.AddEmpty();
  ASSERT_TRUE(static_cast<bool>(created));
  EXPECT_EQ(changes, 1);
  Contact contact = *created;
  contact.local.trust = TrustLevel::Friendly;
  ASSERT_TRUE(static_cast<bool>(store.Upsert(contact)));
  EXPECT_EQ(changes, 2);
  ASSERT_TRUE(static_cast<bool>(store.Remove(contact.id)));
  EXPECT_EQ(changes, 3);
  (void)store.List();
  EXPECT_EQ(changes, 3) << "reads do not notify";
}

TEST_F(ContactsStoreTest, AddEmptyPersists) {
  ContactsStore store(data_dir_.string());

  auto created = store.AddEmpty();
  ASSERT_TRUE(static_cast<bool>(created)) << "add empty failed";
  EXPECT_FALSE(created->id.empty());
  EXPECT_TRUE(created->display_name.empty());
  EXPECT_TRUE(created->server_nickname.empty());
  EXPECT_TRUE(created->ids.empty());
  EXPECT_TRUE(created->multiaddrs.empty());

  ContactsStore reloaded(data_dir_.string());
  auto loaded = reloaded.Get(created->id);
  ASSERT_TRUE(static_cast<bool>(loaded));
  ASSERT_TRUE(loaded->has_value());
  EXPECT_EQ(loaded->value().id, created->id);
  EXPECT_TRUE(loaded->value().display_name.empty());
}

TEST_F(ContactsStoreTest, AddFromDirectoryHitMergesByAccountAndPreservesLocal) {
  ContactsStore store(data_dir_.string());

  mesh::DirectoryHit hit;
  hit.hit_id = "relay:bob";
  hit.display_name = "Bob";
  hit.nickname = "bob";
  hit.account_id = "account:bob";
  hit.ids = {{mesh::ContactIdKind::Account, "account:bob", true}, {mesh::ContactIdKind::RelayUser, "relay:bob", false}};
  hit.multiaddrs = {"/ip4/127.0.0.1/tcp/4001/p2p/12D3KooWBob"};

  auto first = store.AddFromDirectoryHit(hit);
  ASSERT_TRUE(static_cast<bool>(first)) << first.error().message;
  Contact edited = *first;
  edited.local.display_name = "Bobby";
  edited.local.trust = TrustLevel::Friendly;
  SyncContactMirrors(edited);
  ASSERT_TRUE(static_cast<bool>(store.Upsert(edited)));

  hit.nickname = "robert";
  hit.multiaddrs = {"/ip4/10.0.0.1/tcp/4001/p2p/12D3KooWBob"};
  auto second = store.AddFromDirectoryHit(hit);
  ASSERT_TRUE(static_cast<bool>(second)) << second.error().message;
  EXPECT_EQ(second->id, first->id);
  EXPECT_EQ(second->local.display_name, "Bobby");
  EXPECT_EQ(second->local.trust, TrustLevel::Friendly);
  EXPECT_EQ(second->remote.nickname, "robert");
  ASSERT_EQ(second->remote.multiaddrs.size(), 1u);
  EXPECT_EQ(second->remote.multiaddrs[0], "/ip4/10.0.0.1/tcp/4001/p2p/12D3KooWBob");
  EXPECT_GT(second->remote.fetched_at, 0);
}

TEST_F(ContactsStoreTest, AddFromDirectoryHitRequiresAccountId) {
  ContactsStore store(data_dir_.string());
  mesh::DirectoryHit hit;
  hit.hit_id = "relay:bob";
  hit.nickname = "bob";
  hit.ids = {{mesh::ContactIdKind::RelayUser, "relay:bob", true}};
  auto result = store.AddFromDirectoryHit(hit);
  ASSERT_FALSE(static_cast<bool>(result));
  EXPECT_NE(result.error().message.find("Account ID"), std::string::npos);
}

// A contacts file of another version than this build's is refused, older (or unversioned) as newer: pre-release,
// no leftover contacts file is carried over.
TEST_F(ContactsStoreTest, RejectsAnOlderOrUnversionedFile) {
  const auto path = data_dir_ / "contacts.json";
  std::filesystem::create_directories(data_dir_);
  for (const std::optional<int64_t> version : {std::optional<int64_t>{}, std::optional<int64_t>{1}}) {
    {
      Object old;
      if (version) {
        old.set("schema_version", *version);
      }
      old.set("contacts", ArrayValue({}));
      std::ofstream out(path);
      ASSERT_TRUE(static_cast<bool>(out));
      out << DumpJson(old, 2);
    }
    ContactsStore store(data_dir_.string());
    auto listed = store.List();
    ASSERT_FALSE(static_cast<bool>(listed));
    EXPECT_NE(listed.error().message.find("schema"), std::string::npos) << listed.error().message;
  }
}

TEST_F(ContactsStoreTest, RejectsNewerSchemaVersion) {
  const auto path = data_dir_ / "contacts.json";
  std::filesystem::create_directories(data_dir_);
  {
    Object newer;
    newer.set("schema_version", static_cast<int64_t>(ContactsStore::kSchemaVersion + 1));
    newer.set("contacts", ArrayValue({}));
    std::ofstream out(path);
    ASSERT_TRUE(static_cast<bool>(out));
    out << DumpJson(newer, 2);
  }

  ContactsStore store(data_dir_.string());
  auto listed = store.List();
  ASSERT_FALSE(static_cast<bool>(listed));
  EXPECT_NE(listed.error().message.find("schema"), std::string::npos);
}

TEST_F(ContactsStoreTest, LoadsContactsAtProfileJsonFileSizeLimit) {
  Object root;
  root.set("schema_version", static_cast<int64_t>(ContactsStore::kSchemaVersion));
  root.set("contacts", ArrayValue({}));
  root.set("padding", "");
  const std::string base_json = DumpJson(root, 2);
  root.set("padding", std::string(kMaxProfileJsonFileBytes - base_json.size(), 'x'));
  const std::string json = DumpJson(root, 2);
  ASSERT_EQ(json.size(), kMaxProfileJsonFileBytes);

  std::filesystem::create_directories(data_dir_);
  std::ofstream out(data_dir_ / "contacts.json", std::ios::binary);
  ASSERT_TRUE(static_cast<bool>(out));
  out.write(json.data(), static_cast<std::streamsize>(json.size()));
  out.close();

  ContactsStore store(data_dir_.string());
  auto listed = store.List();
  ASSERT_TRUE(static_cast<bool>(listed)) << listed.error().message;
  EXPECT_TRUE(listed->empty());
}

TEST_F(ContactsStoreTest, RejectsContactsOverProfileJsonFileSizeLimit) {
  std::filesystem::create_directories(data_dir_);
  std::ofstream out(data_dir_ / "contacts.json", std::ios::binary);
  ASSERT_TRUE(static_cast<bool>(out));
  std::string oversized(kMaxProfileJsonFileBytes + 1, 'x');
  out.write(oversized.data(), static_cast<std::streamsize>(oversized.size()));
  out.close();

  ContactsStore store(data_dir_.string());
  auto listed = store.List();
  ASSERT_FALSE(static_cast<bool>(listed));
  EXPECT_NE(listed.error().message.find("exceeds limit"), std::string::npos);
}

// A stranger who wrote first is added from a bare id; when the directory later answers, the nickname lands
// in the contact and becomes its title, while the user's own remark stays empty (and editable).
TEST_F(ContactsStoreTest, DirectoryNicknameFillsAContactAddedFromABareId) {
  ContactsStore store(data_dir_.string());

  mesh::DirectoryHit bare;
  bare.hit_id = "account:stranger";
  bare.account_id = "account:stranger";
  bare.ids = {{mesh::ContactIdKind::Account, "account:stranger", true}};
  auto added = store.AddFromDirectoryHit(bare);
  ASSERT_TRUE(static_cast<bool>(added)) << added.error().message;
  EXPECT_TRUE(added->local.display_name.empty());
  EXPECT_TRUE(ContactEffectiveTitle(*added).find("account") != std::string::npos ||
              ContactEffectiveTitle(*added).find("stranger") != std::string::npos)
      << "before the lookup only the id is known: " << ContactEffectiveTitle(*added);

  mesh::DirectoryHit answered = bare;
  answered.nickname = "Ada";
  auto filled = store.ApplyRemoteSnapshot(added->id, answered, 0);
  ASSERT_TRUE(static_cast<bool>(filled)) << filled.error().message;
  EXPECT_EQ(filled->remote.nickname, "Ada");
  EXPECT_TRUE(filled->local.display_name.empty()) << "the remark is the user's; never filled from the directory";
  EXPECT_EQ(ContactEffectiveTitle(*filled), "Ada");
}

TEST_F(ContactsStoreTest, ProfileRefreshChangesOnlyNicknameIconAndFetchTime) {
  ContactsStore store(data_dir_.string());
  mesh::DirectoryHit hit;
  hit.account_id = "account:ada";
  hit.nickname = "Ada";
  hit.ids = {{mesh::ContactIdKind::Account, "account:ada", true}, {mesh::ContactIdKind::RelayUser, "relay:ada", false}};
  hit.multiaddrs = {"/ip4/10.0.0.1/tcp/1/p2p/QmAda"};
  hit.icon = mesh::ProfileIconRef{"https://cdn.example/avatars/old.png", "old", ""};
  auto added = store.AddFromDirectoryHit(hit);
  ASSERT_TRUE(static_cast<bool>(added)) << added.error().message;

  mesh::DirectoryHit fresh;
  fresh.account_id = "account:ada";
  fresh.nickname = "Ada L.";
  fresh.ids = {{mesh::ContactIdKind::Account, "account:ada", true},
               {mesh::ContactIdKind::RelayUser, "relay:other", false}};
  fresh.multiaddrs = {"/ip4/9.9.9.9/tcp/9/p2p/QmElse"};
  fresh.icon = mesh::ProfileIconRef{"https://cdn.example/avatars/new.png", "new", ""};
  auto refreshed = store.ApplyProfileRefresh(added->id, fresh, 12345);
  ASSERT_TRUE(static_cast<bool>(refreshed)) << refreshed.error().message;
  EXPECT_EQ(refreshed->remote.nickname, "Ada L.");
  ASSERT_TRUE(refreshed->remote.icon.has_value());
  EXPECT_EQ(refreshed->remote.icon->url, "https://cdn.example/avatars/new.png");
  EXPECT_EQ(refreshed->remote.fetched_at, 12345);
  EXPECT_EQ(refreshed->id, added->id);
  EXPECT_EQ(refreshed->remote.multiaddrs, added->remote.multiaddrs) << "endpoints are never taken from a refresh";
  ASSERT_EQ(refreshed->remote.ids.size(), added->remote.ids.size());
  for (size_t i = 0; i < added->remote.ids.size(); ++i) {
    EXPECT_EQ(refreshed->remote.ids[i].value, added->remote.ids[i].value);
  }

  fresh.nickname.clear(); // an empty nickname never wipes the known one
  auto kept = store.ApplyProfileRefresh(added->id, fresh, 20000);
  ASSERT_TRUE(static_cast<bool>(kept));
  EXPECT_EQ(kept->remote.nickname, "Ada L.");
}

TEST_F(ContactsStoreTest, ProfileRefreshFromAnotherAccountChangesNothing) {
  ContactsStore store(data_dir_.string());
  mesh::DirectoryHit hit;
  hit.account_id = "account:ada";
  hit.nickname = "Ada";
  hit.ids = {{mesh::ContactIdKind::Account, "account:ada", true}};
  auto added = store.AddFromDirectoryHit(hit);
  ASSERT_TRUE(static_cast<bool>(added));

  mesh::DirectoryHit other = hit;
  other.account_id = "account:mallory";
  other.nickname = "Mallory";
  other.ids = {{mesh::ContactIdKind::Account, "account:mallory", true}};
  other.icon = mesh::ProfileIconRef{"https://cdn.example/avatars/m.png", "m", ""};
  EXPECT_FALSE(static_cast<bool>(store.ApplyProfileRefresh(added->id, other, 999)));
  auto after = store.Get(added->id);
  ASSERT_TRUE(static_cast<bool>(after) && after->has_value());
  EXPECT_EQ((*after)->remote.nickname, "Ada");
  EXPECT_EQ((*after)->remote.fetched_at, added->remote.fetched_at);
  EXPECT_FALSE((*after)->remote.icon.has_value());
}

// A contact added from a QR code: Account ID, a nickname and one device, no relay id.
class RemoteSnapshotTest : public ContactsStoreTest {
protected:
  Contact AddScanned(ContactsStore& store, const bool with_chain = false) {
    mesh::DirectoryHit hit;
    hit.account_id = "account:ada";
    hit.nickname = "Ada";
    hit.ids = {{mesh::ContactIdKind::Account, "account:ada", true},
               {mesh::ContactIdKind::PeerId, "12D3KooWOld", false}};
    if (with_chain) {
      hit.ids.push_back({mesh::ContactIdKind::RelayUser, "relay:ada", false});
    }
    hit.endpoints = {mesh::DirectoryEndpoint{.peer_id = "12D3KooWOld"}};
    hit.multiaddrs = {"/ip4/10.0.0.1/tcp/1/p2p/12D3KooWOld"};
    auto added = store.AddFromDirectoryHit(hit);
    EXPECT_TRUE(static_cast<bool>(added));
    return *added;
  }
  static bool HasId(const Contact& contact, const mesh::ContactIdKind kind, const std::string& value) {
    for (const mesh::ContactId& id : contact.remote.ids) {
      if (id.kind == kind && id.value == value) {
        return true;
      }
    }
    return false;
  }
  static mesh::DirectoryHit Answer() {
    mesh::DirectoryHit hit;
    hit.account_id = "account:ada";
    hit.ids = {{mesh::ContactIdKind::Account, "account:ada", true}};
    return hit;
  }
};

TEST_F(RemoteSnapshotTest, AnotherAccountIsRefusedAndNothingChanges) {
  ContactsStore store(data_dir_.string());
  const Contact before = AddScanned(store);
  mesh::DirectoryHit other = Answer();
  other.account_id = "account:mallory";
  other.ids = {{mesh::ContactIdKind::Account, "account:mallory", true},
               {mesh::ContactIdKind::RelayUser, "relay:m", false}};
  other.nickname = "Mallory";
  auto applied = store.ApplyRemoteSnapshot(before.id, other, 999, std::string("account:ada"));
  ASSERT_FALSE(static_cast<bool>(applied));
  EXPECT_EQ(applied.error().message, "Directory hit is for another account");
  const Contact after = **store.Get(before.id);
  EXPECT_EQ(after.remote.nickname, "Ada");
  EXPECT_FALSE(HasId(after, mesh::ContactIdKind::RelayUser, "relay:m"));
  EXPECT_EQ(after.remote.fetched_at, before.remote.fetched_at);
}

TEST_F(RemoteSnapshotTest, TheRelayIdIsFilledInAndTheDeviceStays) {
  ContactsStore store(data_dir_.string());
  const Contact before = AddScanned(store);
  mesh::DirectoryHit hit = Answer();
  hit.ids.push_back({mesh::ContactIdKind::RelayUser, "relay:ada", false});
  auto applied = store.ApplyRemoteSnapshot(before.id, hit, 5000);
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::RelayUser, "relay:ada"));
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::PeerId, "12D3KooWOld"));
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::Account, "account:ada"));
  EXPECT_EQ(applied->remote.endpoints.size(), 1u);
  EXPECT_EQ(applied->remote.multiaddrs, before.remote.multiaddrs);
  EXPECT_EQ(applied->remote.nickname, "Ada") << "an empty nickname keeps the known one";
  EXPECT_EQ(applied->remote.fetched_at, 5000);
}

TEST_F(RemoteSnapshotTest, ANewDeviceReplacesTheOldOneWithItsAddresses) {
  ContactsStore store(data_dir_.string());
  const Contact before = AddScanned(store);
  mesh::DirectoryHit hit = Answer();
  hit.ids.push_back({mesh::ContactIdKind::PeerId, "12D3KooWNew", false});
  hit.endpoints = {mesh::DirectoryEndpoint{.peer_id = "12D3KooWNew", .multiaddrs = {"/ip4/9.9.9.9/tcp/9"}}};
  hit.multiaddrs = {"/ip4/9.9.9.9/tcp/9"};
  auto applied = store.ApplyRemoteSnapshot(before.id, hit, 5000);
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_FALSE(HasId(*applied, mesh::ContactIdKind::PeerId, "12D3KooWOld"));
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::PeerId, "12D3KooWNew"));
  ASSERT_EQ(applied->remote.endpoints.size(), 1u);
  EXPECT_EQ(applied->remote.endpoints[0].peer_id, "12D3KooWNew");
  EXPECT_EQ(applied->remote.multiaddrs, hit.multiaddrs);
}

TEST_F(RemoteSnapshotTest, ANewDeviceWithoutAddressesClearsTheOldAddresses) {
  ContactsStore store(data_dir_.string());
  const Contact before = AddScanned(store);
  mesh::DirectoryHit hit = Answer();
  hit.ids.push_back({mesh::ContactIdKind::PeerId, "12D3KooWNew", false});
  auto applied = store.ApplyRemoteSnapshot(before.id, hit, 5000);
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_FALSE(HasId(*applied, mesh::ContactIdKind::PeerId, "12D3KooWOld"));
  EXPECT_TRUE(applied->remote.endpoints.empty());
  EXPECT_TRUE(applied->remote.multiaddrs.empty());
}

TEST_F(RemoteSnapshotTest, AnEmptyAnswerKeepsRouteNicknameDeviceAndAddresses) {
  ContactsStore store(data_dir_.string());
  const Contact before = AddScanned(store, true);
  auto applied = store.ApplyRemoteSnapshot(before.id, Answer(), 5000);
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::RelayUser, "relay:ada"));
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::PeerId, "12D3KooWOld"));
  EXPECT_EQ(applied->remote.nickname, "Ada");
  EXPECT_EQ(applied->remote.endpoints.size(), before.remote.endpoints.size());
  EXPECT_EQ(applied->remote.multiaddrs, before.remote.multiaddrs);
}

TEST_F(RemoteSnapshotTest, ARelayOnlyContactIsOverwrittenAsAWhole) {
  ContactsStore store(data_dir_.string());
  mesh::DirectoryHit old_hit;
  old_hit.nickname = "Old";
  old_hit.ids = {{mesh::ContactIdKind::RelayUser, "relay:old", true},
                 {mesh::ContactIdKind::PeerId, "12D3KooWOld", false}};
  old_hit.multiaddrs = {"/ip4/10.0.0.1/tcp/1"};
  Contact contact;
  contact.id = "c-relay-only";
  contact.remote.ids = old_hit.ids;
  contact.remote.nickname = "Old";
  contact.remote.multiaddrs = old_hit.multiaddrs;
  ASSERT_TRUE(static_cast<bool>(store.Upsert(contact)));

  mesh::DirectoryHit fresh;
  fresh.ids = {{mesh::ContactIdKind::RelayUser, "relay:old", true},
               {mesh::ContactIdKind::PeerId, "12D3KooWNew", false}};
  auto applied = store.ApplyRemoteSnapshot(contact.id, fresh, 5000);
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_FALSE(HasId(*applied, mesh::ContactIdKind::PeerId, "12D3KooWOld"));
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::PeerId, "12D3KooWNew"));
  EXPECT_TRUE(applied->remote.nickname.empty());
  EXPECT_TRUE(applied->remote.multiaddrs.empty());
}

TEST_F(RemoteSnapshotTest, AnAnswerWithoutAnAccountIdIsAcceptedOnlyForAByAccountLookup) {
  ContactsStore store(data_dir_.string());
  const Contact before = AddScanned(store);
  mesh::DirectoryHit hit;
  hit.ids = {{mesh::ContactIdKind::RelayUser, "relay:ada", true}};
  EXPECT_FALSE(static_cast<bool>(store.ApplyRemoteSnapshot(before.id, hit, 5000)));
  EXPECT_FALSE(HasId(**store.Get(before.id), mesh::ContactIdKind::RelayUser, "relay:ada"));
  auto applied = store.ApplyRemoteSnapshot(before.id, hit, 5000, std::string("account:ada"));
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::RelayUser, "relay:ada"));
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::Account, "account:ada")) << "the account id stays";
  EXPECT_FALSE(static_cast<bool>(store.ApplyRemoteSnapshot(before.id, hit, 5000, std::string("account:other"))));
}

TEST_F(RemoteSnapshotTest, AForeignAccountIdInTheAnswerIsNotStored) {
  ContactsStore store(data_dir_.string());
  const Contact before = AddScanned(store);
  mesh::DirectoryHit hit = Answer();
  hit.ids = {{mesh::ContactIdKind::Account, "account:mallory", true}};
  auto applied = store.ApplyRemoteSnapshot(before.id, hit, 5000, std::string("account:ada"));
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_FALSE(HasId(*applied, mesh::ContactIdKind::Account, "account:mallory"));
  EXPECT_EQ(ContactAccountId(*applied), std::optional<std::string>("account:ada"));
  auto found = store.FindByIdentity("account:mallory", mesh::ContactIdKind::Account);
  ASSERT_TRUE(static_cast<bool>(found));
  EXPECT_FALSE(found->has_value());

  mesh::DirectoryHit both = Answer();
  both.ids = {{mesh::ContactIdKind::Account, "account:mallory", true},
              {mesh::ContactIdKind::Account, "account:ada", false}};
  applied = store.ApplyRemoteSnapshot(before.id, both, 6000, std::string("account:ada"));
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_FALSE(HasId(*applied, mesh::ContactIdKind::Account, "account:mallory"));
  ASSERT_FALSE(applied->remote.ids.empty());
  EXPECT_EQ(applied->remote.ids.front().kind, mesh::ContactIdKind::Account);
  EXPECT_EQ(applied->remote.ids.front().value, "account:ada");
  EXPECT_TRUE(applied->remote.ids.front().primary);
  size_t accounts = 0;
  for (const mesh::ContactId& id : applied->remote.ids) {
    accounts += id.kind == mesh::ContactIdKind::Account ? 1 : 0;
  }
  EXPECT_EQ(accounts, 1u);
  found = store.FindByIdentity("account:mallory", mesh::ContactIdKind::Account);
  ASSERT_TRUE(static_cast<bool>(found));
  EXPECT_FALSE(found->has_value());
}

TEST_F(RemoteSnapshotTest, KeptIdsKeepTheirPrimaryFlagAndOtherKindsStay) {
  ContactsStore store(data_dir_.string());
  Contact contact;
  contact.id = "c-primary";
  contact.remote.nickname = "Ada";
  contact.remote.ids = {{mesh::ContactIdKind::Account, "account:ada", true},
                        {mesh::ContactIdKind::PeerId, "12D3KooWOld", true},
                        {mesh::ContactIdKind::Blockchain, "0xada", true}};
  auto added = store.Upsert(contact);
  ASSERT_TRUE(static_cast<bool>(added)) << added.error().message;
  auto applied = store.ApplyRemoteSnapshot(added->id, Answer(), 5000, std::string("account:ada"));
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  bool saw_peer = false;
  bool saw_chain = false;
  for (const mesh::ContactId& id : applied->remote.ids) {
    if (id.kind == mesh::ContactIdKind::PeerId && id.value == "12D3KooWOld") {
      saw_peer = true;
      EXPECT_TRUE(id.primary);
    }
    if (id.kind == mesh::ContactIdKind::Blockchain && id.value == "0xada") {
      saw_chain = true;
      EXPECT_TRUE(id.primary);
    }
  }
  EXPECT_TRUE(saw_peer);
  EXPECT_TRUE(saw_chain);

  // An answer that has its own primary of that kind wins; the kept one is demoted.
  mesh::DirectoryHit with_chain = Answer();
  with_chain.ids.push_back({mesh::ContactIdKind::Blockchain, "0xnew", true});
  applied = store.ApplyRemoteSnapshot(added->id, with_chain, 6000, std::string("account:ada"));
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::Blockchain, "0xnew"));
  EXPECT_FALSE(HasId(*applied, mesh::ContactIdKind::Blockchain, "0xada")) << "the answer has that kind";
}

TEST_F(ContactsStoreTest, ProfileRefreshAdoptsOnlyTheRelayIdWhenAsked) {
  ContactsStore store(data_dir_.string());
  mesh::DirectoryHit hit;
  hit.account_id = "account:ada";
  hit.ids = {{mesh::ContactIdKind::Account, "account:ada", true}, {mesh::ContactIdKind::PeerId, "12D3KooWOld", false}};
  auto added = store.AddFromDirectoryHit(hit);
  ASSERT_TRUE(static_cast<bool>(added));
  mesh::DirectoryHit answer;
  answer.ids = {{mesh::ContactIdKind::RelayUser, "relay:ada", false},
                {mesh::ContactIdKind::PeerId, "12D3KooWNew", false}};
  answer.multiaddrs = {"/ip4/9.9.9.9/tcp/9"};
  auto plain = store.ApplyProfileRefresh(added->id, answer, 100, std::string("account:ada"));
  ASSERT_TRUE(static_cast<bool>(plain)) << plain.error().message;
  EXPECT_FALSE(ContactRelayUserId(*plain).has_value());
  auto adopted = store.ApplyProfileRefresh(added->id, answer, 200, std::string("account:ada"), true);
  ASSERT_TRUE(static_cast<bool>(adopted)) << adopted.error().message;
  EXPECT_EQ(ContactRelayUserId(*adopted), std::optional<std::string>("relay:ada"));
  EXPECT_EQ(adopted->remote.ids.size(), added->remote.ids.size() + 1);
  EXPECT_TRUE(adopted->remote.multiaddrs.empty());
}

// By-account answers shaped as docs/contracts/SERVICE_ENDPOINTS.md describes them.
TEST_F(RemoteSnapshotTest, AParsedByAccountAnswerWithARelayIdFillsItIn) {
  ContactsStore store(data_dir_.string());
  const Contact before = AddScanned(store);
  auto json = TryParseObject(R"({"account_id": "account:ada", "relay_user_id": "relay:ada", "nickname": "Ada",
                                 "signature_alg": "ml-dsa-65", "endpoints": []})");
  ASSERT_TRUE(json.has_value());
  const mesh::DirectoryHit hit = DirectoryHitFromJson(*json);
  auto applied = store.ApplyRemoteSnapshot(before.id, hit, 5000, std::string("account:ada"));
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::RelayUser, "relay:ada"));
  EXPECT_TRUE(HasId(*applied, mesh::ContactIdKind::PeerId, "12D3KooWOld"));
}

TEST_F(RemoteSnapshotTest, AParsedByAccountAnswerWithoutARelayIdLeavesTheRouteEmpty) {
  ContactsStore store(data_dir_.string());
  const Contact before = AddScanned(store);
  auto json = TryParseObject(R"({"account_id": "account:ada", "nickname": "Ada", "signature_alg": "ml-dsa-65"})");
  ASSERT_TRUE(json.has_value());
  const mesh::DirectoryHit hit = DirectoryHitFromJson(*json);
  auto applied = store.ApplyRemoteSnapshot(before.id, hit, 5000, std::string("account:ada"));
  ASSERT_TRUE(static_cast<bool>(applied)) << applied.error().message;
  for (const mesh::ContactId& id : applied->remote.ids) {
    EXPECT_NE(id.kind, mesh::ContactIdKind::RelayUser);
  }
}

TEST_F(ContactsStoreTest, AddingAContactLeavesTheRemarkEmpty) {
  ContactsStore store(data_dir_.string());
  mesh::DirectoryHit hit;
  hit.hit_id = "account:ada";
  hit.display_name = "Ada L.";
  hit.nickname = "Ada";
  hit.account_id = "account:ada";
  hit.ids = {{mesh::ContactIdKind::Account, "account:ada", true}};
  auto added = store.AddFromDirectoryHit(hit);
  ASSERT_TRUE(static_cast<bool>(added)) << added.error().message;
  EXPECT_TRUE(added->local.display_name.empty());
  EXPECT_TRUE(added->display_name.empty());
  EXPECT_EQ(ContactEffectiveTitle(*added), "Ada");

  // A later nickname change shows through, because no remark pins the old one.
  hit.nickname = "Ada Lovelace";
  auto renamed = store.ApplyRemoteSnapshot(added->id, hit, 0);
  ASSERT_TRUE(static_cast<bool>(renamed)) << renamed.error().message;
  EXPECT_EQ(ContactEffectiveTitle(*renamed), "Ada Lovelace");
}

TEST(SupportContactTest, NewSupportContactHasNoRemarkAndIsNamedByNickname) {
  const Contact contact = MakeSupportContact("c1", "account:support", "PP Support");
  EXPECT_TRUE(contact.local.display_name.empty());
  EXPECT_EQ(contact.remote.nickname, "PP Support");
  EXPECT_EQ(contact.server_nickname, "PP Support");
  EXPECT_EQ(ContactEffectiveTitle(contact), "PP Support");
}

TEST(SupportContactTest, OpeningTheEntryKeepsAUserRemarkAndRefreshesTheTitle) {
  Contact contact = MakeSupportContact("c1", "account:support", "Old title");
  contact.local.display_name = "My helper";
  SyncContactMirrors(contact);
  EXPECT_TRUE(ApplySupportTitle(contact, "PP Support"));
  EXPECT_EQ(contact.local.display_name, "My helper");
  EXPECT_EQ(contact.display_name, "My helper");
  EXPECT_EQ(contact.remote.nickname, "PP Support");
  EXPECT_EQ(ContactEffectiveTitle(contact), "My helper");
  EXPECT_FALSE(ApplySupportTitle(contact, "PP Support"));
}

// A remark equal to the title is the user's like any other.
TEST(SupportContactTest, ARemarkEqualToTheTitleStays) {
  Contact contact = MakeSupportContact("c1", "account:support", "PP Support");
  contact.local.display_name = "PP Support";
  SyncContactMirrors(contact);
  EXPECT_FALSE(ApplySupportTitle(contact, "PP Support"));
  EXPECT_EQ(contact.local.display_name, "PP Support");
}

} // namespace
