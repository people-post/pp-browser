#include "feature/conversations/AccountLockerWorkflow.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/RecoverySeed.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "common/Logger.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** The relay as a locker provider: keeps the newest version per locker; can be made unreachable. */
class FakeLockerProvider final : public ILockerClient {
public:
  Roe<void> Put(const std::string&, const LockerRecord& record, std::optional<LockerPutRefusal>* refused) override {
    auto held = records_.find(record.locker_id);
    if (held != records_.end() && held->second.version >= record.version) {
      if (refused) {
        *refused = LockerPutRefusal::NotNewer;
      }
      return Error("not newer");
    }
    records_[record.locker_id] = record;
    return {};
  }
  Roe<std::optional<LockerRecord>> Get(const std::string& locker_id) override {
    if (unreachable) {
      return Error("connection refused");
    }
    auto held = records_.find(locker_id);
    return held == records_.end() ? std::optional<LockerRecord>{} : std::optional<LockerRecord>(held->second);
  }
  bool unreachable = false;

private:
  std::map<std::string, LockerRecord> records_;
};

Contact AccountContact(const std::string& id, const std::string& account, const std::string& name) {
  Contact contact;
  contact.id = id;
  contact.remote.ids.push_back({mesh::ContactIdKind::Account, account, true});
  contact.local.display_name = name;
  SyncContactMirrors(contact);
  return contact;
}

/** One device of Alice's account (a seed, a relay id, Bob in the address book) and its locker workflow. */
class AccountLockerWorkflowTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_locker_workflow_" + util::GenerateUuid());
    std::filesystem::create_directories(dir_);
    identity_ = std::make_unique<IdentityStore>(dir_.string(), "alice");
    ASSERT_TRUE(identity_->SetDek(ByteVector(kDataEncryptionKeySize, 0x42)));
    auto minted = identity_->LoadOrCreate();
    ASSERT_TRUE(minted);
    LocalIdentity registered = *minted;
    registered.registered = true;
    registered.nickname = "alice";
    registered.registered = true;
    ASSERT_TRUE(identity_->Update(registered));
    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    ASSERT_TRUE(contacts_->Upsert(AccountContact("c1", "account:bob", "Bob")));
    workflow_ = std::make_unique<AccountLockerWorkflow>(*identity_, *contacts_, AccountLockerWorkflow::Sources{});
    workflow_->SetClient(provider_);
    auto seed = identity_->RecoverySeed();
    ASSERT_TRUE(seed && *seed);
    seed_ = **seed;
  }
  void TearDown() override {
    workflow_.reset();
    contacts_.reset();
    identity_.reset();
    std::filesystem::remove_all(dir_);
  }

  void RenameBob(const std::string& name) { ASSERT_TRUE(contacts_->Upsert(AccountContact("c1", "account:bob", name))); }

  /** What a fresh install restoring from the phrase finds (a workflow of its own: nothing seen yet). */
  Roe<std::optional<AccountLockerWorkflow::Fetched>> Restore(const std::vector<std::string>& saved_copies) {
    AccountLockerWorkflow restoring(*identity_, *contacts_, AccountLockerWorkflow::Sources{});
    restoring.SetClient(provider_);
    return restoring.Fetch(seed_, saved_copies);
  }

  std::filesystem::path dir_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<AccountLockerWorkflow> workflow_;
  std::shared_ptr<FakeLockerProvider> provider_ = std::make_shared<FakeLockerProvider>();
  ByteVector seed_;
};

std::string BobsName(const AccountLockerWorkflow::Fetched& fetched) {
  for (const Contact& contact : fetched.state.contacts) {
    if (contact.id == "c1") {
      return contact.local.display_name;
    }
  }
  return {};
}

// The grants given to the account (multi-relay MR005) go into the locker from the device holding them and are
// handed to another device at its sync and to a restore, so their first sends to the grantors are not a stranger's.
TEST_F(AccountLockerWorkflowTest, GrantsGoIntoTheLockerAndAnotherDeviceTakesThemAtItsSync) {
  const int64_t now = util::NowUnixMs();
  const AccountStateGrant bobs{.grantor_account_id = "account:bob",
                               .grantee_account_id = "account:alice",
                               .issued_at = now,
                               .expires_at = now + 86'400'000,
                               .signature = "c2ln"};
  const AccountStateGrant expired{.grantor_account_id = "account:carol",
                                  .grantee_account_id = "account:alice",
                                  .issued_at = now - 2'000,
                                  .expires_at = now - 1'000,
                                  .signature = "c2ln"};
  AccountLockerWorkflow holder(*identity_, *contacts_,
                               AccountLockerWorkflow::Sources{.grants = [&]() -> Roe<std::vector<AccountStateGrant>> {
                                 return std::vector<AccountStateGrant>{bobs, expired};
                               }});
  holder.SetClient(provider_);
  ASSERT_TRUE(holder.Publish());

  auto restored = Restore({});
  ASSERT_TRUE(restored && restored->has_value());
  EXPECT_EQ((**restored).state.extras.grants, std::vector<AccountStateGrant>{bobs})
      << "a restore brings it, not the expired";

  std::vector<AccountStateGrant> taken;
  AccountLockerWorkflow linked(
      *identity_, *contacts_,
      AccountLockerWorkflow::Sources{
          .take_grants = [&](const std::vector<AccountStateGrant>& grants) { taken = grants; }});
  linked.SetClient(provider_);
  ASSERT_TRUE(linked.Publish());
  EXPECT_EQ(taken, std::vector<AccountStateGrant>{bobs}) << "another device takes it at its sync";
  auto after = Restore({});
  ASSERT_TRUE(after && after->has_value());
  EXPECT_EQ((**after).state.extras.grants, std::vector<AccountStateGrant>{bobs})
      << "a device holding none does not drop it from the locker";
}

TEST_F(AccountLockerWorkflowTest, ASavedFileRestoresTheAccountWhenTheProviderHasNone) {
  auto file = workflow_->ExportFileJson();
  ASSERT_TRUE(file) << file.error().message;
  auto restored = Restore({*file});
  ASSERT_TRUE(restored) << restored.error().message;
  ASSERT_TRUE(restored->has_value());
  EXPECT_EQ(BobsName(**restored), "Bob");
}

TEST_F(AccountLockerWorkflowTest, TheNewestCopyWinsWhetherFileOrProvider) {
  auto old_file = workflow_->ExportFileJson();
  ASSERT_TRUE(old_file);
  RenameBob("Bob from work");
  ASSERT_TRUE(workflow_->Publish());
  auto restored = Restore({*old_file});
  ASSERT_TRUE(restored && restored->has_value());
  EXPECT_EQ(BobsName(**restored), "Bob from work") << "the provider's copy is newer than the file";

  RenameBob("Bob (climbing)");
  auto new_file = workflow_->ExportFileJson(); // saved after the last write: newer than the provider's
  ASSERT_TRUE(new_file);
  restored = Restore({*new_file});
  ASSERT_TRUE(restored && restored->has_value());
  EXPECT_EQ(BobsName(**restored), "Bob (climbing)") << "the file is newer than the provider's copy";
}

TEST_F(AccountLockerWorkflowTest, TheProviderGivenIsTheWorkflowsOwnUntilReplaced) {
  // The org's registration URL changes on the hub's thread while a sync runs on a worker: the sync keeps the client
  // it took, not a pointer the change frees.
  std::weak_ptr<FakeLockerProvider> given = provider_;
  provider_.reset();
  EXPECT_FALSE(given.expired()) << "held by the workflow";
  ASSERT_TRUE(workflow_->Publish());
  workflow_->SetClient(nullptr);
  EXPECT_TRUE(given.expired()) << "and let go once replaced";
  EXPECT_FALSE(workflow_->Publish()) << "no provider now";
}

TEST_F(AccountLockerWorkflowTest, AnUnreachableProviderIsNoErrorWithASavedFile) {
  auto file = workflow_->ExportFileJson();
  ASSERT_TRUE(file);
  provider_->unreachable = true;
  EXPECT_FALSE(Restore({})) << "no file: the provider's error";
  auto restored = Restore({*file});
  ASSERT_TRUE(restored) << restored.error().message;
  ASSERT_TRUE(restored->has_value());
  EXPECT_EQ(BobsName(**restored), "Bob");
}

TEST_F(AccountLockerWorkflowTest, RefusesAFileThatIsNotThisAccounts) {
  EXPECT_FALSE(Restore({"{\"hello\":1}"})) << "not a saved account";
  auto file = workflow_->ExportFileJson();
  ASSERT_TRUE(file);
  AccountLockerWorkflow stranger(*identity_, *contacts_, AccountLockerWorkflow::Sources{});
  EXPECT_FALSE(stranger.Fetch(GenerateRecoverySeed(), {*file})) << "another phrase cannot use Alice's file";
}

TEST_F(AccountLockerWorkflowTest, AnExportedVersionKeepsTheProvidersNextWriteAboveIt) {
  auto file = workflow_->ExportFileJson();
  ASSERT_TRUE(file);
  auto record = LockerRecordFromFileJson(*file);
  ASSERT_TRUE(record);
  RenameBob("Bob from work");
  auto written = workflow_->Publish();
  ASSERT_TRUE(written) << written.error().message;
  EXPECT_GT(*written, record->version);
}

// --- Device names ------------------------------------------------------------------------------------------

/** A device of the account for the names tests: its PeerId, the card's list as it knows it, the system's name. */
struct NamedDevice {
  std::string self;
  std::vector<std::string> card;
  std::string system_name;
  int writes = 0;
  std::unique_ptr<AccountLockerWorkflow> workflow;
};

/** Collects every line of the loggers it is added to (handlers cannot be removed: kept for the process). */
struct Lines : logging::Handler {
  void emit(logging::Level, const std::string&, const std::string& message) override {
    std::lock_guard lock(mu);
    all += message + "\n";
  }
  std::string Text() {
    std::lock_guard lock(mu);
    return all;
  }
  std::mutex mu;
  std::string all;
};

class AccountLockerWorkflowNamesTest : public AccountLockerWorkflowTest {
protected:
  /** Another device of the same account: same seed, address book and provider; its own table and card. */
  std::unique_ptr<NamedDevice> Device(const std::string& self, std::vector<std::string> card,
                                      const std::string& system_name) {
    auto device = std::make_unique<NamedDevice>();
    device->self = self;
    device->card = std::move(card);
    device->system_name = system_name;
    NamedDevice* raw = device.get();
    device->workflow = std::make_unique<AccountLockerWorkflow>(
        *identity_, *contacts_,
        AccountLockerWorkflow::Sources{.own_devices = [raw]() -> Roe<OwnDeviceList> {
                                         OwnDeviceList list;
                                         list.self_peer_id = raw->self;
                                         for (const std::string& peer : raw->card) {
                                           AccountCardDevice listed;
                                           listed.peer_id = peer;
                                           list.devices.push_back(listed);
                                         }
                                         return list;
                                       },
                                       .own_system_name = [raw]() { return raw->system_name; }});
    device->workflow->SetClient(provider_);
    device->workflow->SetOnWritten([raw]() { ++raw->writes; });
    return device;
  }

  std::string NameIn(const std::vector<DeviceNameEntry>& names, const std::string& peer) {
    for (const DeviceNameEntry& entry : names) {
      if (entry.peer_id == peer) {
        return entry.name;
      }
    }
    return "";
  }

  std::string NameOnProvider(const std::string& peer) {
    auto fetched = Restore({});
    EXPECT_TRUE(fetched && fetched->has_value());
    return fetched && fetched->has_value() ? NameIn((**fetched).state.device_names, peer) : std::string{};
  }
};

TEST_F(AccountLockerWorkflowNamesTest, TwoPublishesWithNothingChangedWriteOnceAndNudgeOnce) {
  auto phone = Device("peer:phone", {"peer:phone"}, "iPhone");
  ASSERT_TRUE(phone->workflow->Publish());
  EXPECT_EQ(phone->writes, 1);
  EXPECT_EQ(NameOnProvider("peer:phone"), "iPhone");
  ASSERT_TRUE(phone->workflow->Publish());
  EXPECT_EQ(phone->writes, 1) << "nothing changed: no write, no sibling nudge";
  auto laptop = Device("peer:laptop", {"peer:phone", "peer:laptop"}, "MacBook");
  ASSERT_TRUE(laptop->workflow->Publish());
  EXPECT_EQ(laptop->writes, 1) << "its own name is new";
  ASSERT_TRUE(laptop->workflow->Publish());
  ASSERT_TRUE(phone->workflow->Publish());
  ASSERT_TRUE(laptop->workflow->Publish());
  ASSERT_TRUE(phone->workflow->Publish());
  EXPECT_EQ(laptop->writes, 1);
  EXPECT_EQ(phone->writes, 1) << "the phone took the laptop's name without rewriting the locker";
  EXPECT_EQ(NameIn(phone->workflow->DeviceNames(), "peer:laptop"), "MacBook");
  EXPECT_EQ(NameIn(laptop->workflow->DeviceNames(), "peer:phone"), "iPhone");
}

TEST_F(AccountLockerWorkflowNamesTest, TheMainDevicesNameForAnotherSticksAndTheSystemNameNeverOverwritesIt) {
  auto phone = Device("peer:phone", {"peer:phone", "peer:laptop"}, "iPhone");
  ASSERT_TRUE(phone->workflow->Publish());
  // The main device names the laptop before the laptop has ever published.
  ASSERT_TRUE(phone->workflow->SetDeviceName("peer:laptop", "Office Mac", util::NowUnixMs()));
  ASSERT_TRUE(phone->workflow->Publish());
  auto laptop = Device("peer:laptop", {"peer:phone", "peer:laptop"}, "MacBook");
  ASSERT_TRUE(laptop->workflow->Publish());
  EXPECT_EQ(NameIn(laptop->workflow->DeviceNames(), "peer:laptop"), "Office Mac") << "no default over a given name";
  laptop->system_name = "Renamed in the system";
  ASSERT_TRUE(laptop->workflow->Publish());
  EXPECT_EQ(NameOnProvider("peer:laptop"), "Office Mac");
  // And the laptop renaming itself reaches the phone, which keeps it through its own publishes.
  ASSERT_TRUE(laptop->workflow->SetDeviceName("peer:laptop", "Mac Bureau", util::NowUnixMs() + 10));
  ASSERT_TRUE(laptop->workflow->Publish());
  ASSERT_TRUE(phone->workflow->Publish());
  EXPECT_EQ(NameIn(phone->workflow->DeviceNames(), "peer:laptop"), "Mac Bureau");
  ASSERT_TRUE(phone->workflow->Publish());
  EXPECT_EQ(NameOnProvider("peer:laptop"), "Mac Bureau");
}

TEST_F(AccountLockerWorkflowNamesTest, ANewlyLinkedDeviceGetsEveryNameOnItsFirstSync) {
  auto phone = Device("peer:phone", {"peer:phone"}, "iPhone");
  ASSERT_TRUE(phone->workflow->Publish());
  auto tablet = Device("peer:tablet", {"peer:phone", "peer:tablet"}, "iPad");
  ASSERT_TRUE(tablet->workflow->Publish());
  EXPECT_EQ(NameIn(tablet->workflow->DeviceNames(), "peer:phone"), "iPhone");
  EXPECT_EQ(NameIn(tablet->workflow->DeviceNames(), "peer:tablet"), "iPad");
  ASSERT_TRUE(phone->workflow->Publish());
  EXPECT_EQ(NameIn(phone->workflow->DeviceNames(), "peer:tablet"), "iPad");
}

TEST_F(AccountLockerWorkflowNamesTest, AnEntryMadeEmptyOrUnchangedWritesNothing) {
  auto phone = Device("peer:phone", {"peer:phone"}, "iPhone");
  ASSERT_TRUE(phone->workflow->Publish());
  const auto before = phone->workflow->DeviceNames();
  EXPECT_FALSE(phone->workflow->SetDeviceName("peer:phone", "  \n ", util::NowUnixMs())) << "empty input is a cancel";
  EXPECT_FALSE(phone->workflow->SetDeviceName("peer:phone", "iPhone", util::NowUnixMs() + 99)) << "same name: at stays";
  EXPECT_EQ(phone->workflow->DeviceNames(), before);
}

TEST_F(AccountLockerWorkflowNamesTest, ASlowClockCannotBeOverwrittenByTheOldEntry) {
  auto phone = Device("peer:phone", {"peer:phone"}, "iPhone");
  const int64_t hour = 3'600'000;
  const int64_t now = util::NowUnixMs();
  ASSERT_TRUE(phone->workflow->SetDeviceName("peer:phone", "First", now + hour));
  ASSERT_TRUE(phone->workflow->SetDeviceName("peer:phone", "Second", now)); // this clock is an hour behind
  const auto names = phone->workflow->DeviceNames();
  ASSERT_EQ(names.size(), 1u);
  EXPECT_EQ(names[0].name, "Second");
  EXPECT_EQ(names[0].at, now + hour + 1);
}

TEST_F(AccountLockerWorkflowNamesTest, ALockerWrittenWithoutTheKeyCostsNoLocalNameAndIsRepaired) {
  auto phone = Device("peer:phone", {"peer:phone", "peer:laptop"}, "iPhone");
  ASSERT_TRUE(phone->workflow->Publish());
  ASSERT_TRUE(phone->workflow->SetDeviceName("peer:laptop", "Office Mac", util::NowUnixMs()));
  ASSERT_TRUE(phone->workflow->Publish());
  // An older version rewrites the locker: its state has no device_names.
  auto fetched = Restore({});
  ASSERT_TRUE(fetched && fetched->has_value());
  AccountLockerState old_state = (**fetched).state;
  old_state.device_names.clear();
  auto recovery = DeriveRecoveryKeys(seed_);
  ASSERT_TRUE(recovery);
  auto keys = account_locker::KeysFrom(*recovery);
  ASSERT_TRUE(keys);
  auto snapshot = account_locker::Seal(old_state, *keys, (**fetched).version + 10);
  ASSERT_TRUE(snapshot);
  ASSERT_TRUE(provider_->Put("account", LockerRecordFromSnapshot(*snapshot), nullptr));
  EXPECT_EQ(NameOnProvider("peer:laptop"), "");

  ASSERT_TRUE(phone->workflow->Publish());
  EXPECT_EQ(NameOnProvider("peer:laptop"), "Office Mac") << "the table kept here went back in";
  EXPECT_EQ(NameOnProvider("peer:phone"), "iPhone");
}

TEST_F(AccountLockerWorkflowNamesTest, AnOlderCardNeverCostsTheNewDevicesNameAndTheRoundSettles) {
  auto phone = Device("peer:phone", {"peer:phone", "peer:tablet"}, "iPhone");
  ASSERT_TRUE(phone->workflow->Publish());
  ASSERT_TRUE(phone->workflow->SetDeviceName("peer:tablet", "Tablet by phone", util::NowUnixMs()));
  ASSERT_TRUE(phone->workflow->Publish());
  // The laptop's card is older: it does not list the tablet.
  auto laptop = Device("peer:laptop", {"peer:phone", "peer:laptop"}, "MacBook");
  ASSERT_TRUE(laptop->workflow->Publish());
  EXPECT_EQ(NameOnProvider("peer:tablet"), "Tablet by phone") << "not pruned by the laptop's card";
  ASSERT_TRUE(phone->workflow->Publish());
  const int phone_writes = phone->writes;
  const int laptop_writes = laptop->writes;
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(laptop->workflow->Publish());
    ASSERT_TRUE(phone->workflow->Publish());
  }
  EXPECT_EQ(phone->writes, phone_writes) << "no more rewriting";
  EXPECT_EQ(laptop->writes, laptop_writes) << "no more rewriting";
  EXPECT_EQ(NameOnProvider("peer:tablet"), "Tablet by phone");
}

TEST_F(AccountLockerWorkflowNamesTest, ARemovedDevicesNameLeavesTheNextPublishAndNoOtherEntryDoes) {
  auto phone = Device("peer:phone", {"peer:phone", "peer:laptop", "peer:tablet"}, "iPhone");
  ASSERT_TRUE(phone->workflow->SetDeviceName("peer:laptop", "Mac", util::NowUnixMs()));
  ASSERT_TRUE(phone->workflow->SetDeviceName("peer:tablet", "iPad", util::NowUnixMs()));
  ASSERT_TRUE(phone->workflow->Publish());
  phone->card = {"peer:phone", "peer:tablet"};
  phone->workflow->ForgetDeviceName("peer:laptop", util::NowUnixMs() + 50);
  ASSERT_TRUE(phone->workflow->Publish());
  EXPECT_EQ(NameOnProvider("peer:laptop"), "");
  EXPECT_EQ(NameOnProvider("peer:tablet"), "iPad");
  EXPECT_EQ(NameOnProvider("peer:phone"), "iPhone");
  EXPECT_EQ(NameIn(phone->workflow->DeviceNames(), "peer:laptop"), "");
}

TEST_F(AccountLockerWorkflowNamesTest, ARemovalTombstoneStopsAThirdDevicePuttingTheNameBack) {
  auto phone = Device("peer:phone", {"peer:phone", "peer:laptop", "peer:tablet"}, "iPhone");
  auto laptop = Device("peer:laptop", {"peer:phone", "peer:laptop", "peer:tablet"}, "MacBook");
  auto tablet = Device("peer:tablet", {"peer:phone", "peer:laptop", "peer:tablet"}, "iPad");
  ASSERT_TRUE(phone->workflow->Publish());
  ASSERT_TRUE(laptop->workflow->Publish());
  ASSERT_TRUE(tablet->workflow->Publish());
  ASSERT_TRUE(phone->workflow->Publish());
  ASSERT_EQ(NameIn(tablet->workflow->DeviceNames(), "peer:laptop"), "MacBook") << "the tablet has the laptop locally";
  // The phone removes the laptop.
  phone->card = {"peer:phone", "peer:tablet"};
  phone->workflow->ForgetDeviceName("peer:laptop", util::NowUnixMs() + 10);
  ASSERT_TRUE(phone->workflow->Publish());
  EXPECT_EQ(NameOnProvider("peer:laptop"), "");
  // The tablet still lists the laptop locally and syncs.
  tablet->card = {"peer:phone", "peer:tablet"};
  const int phone_writes = phone->writes;
  const int tablet_writes = tablet->writes;
  ASSERT_TRUE(tablet->workflow->Publish());
  EXPECT_EQ(NameOnProvider("peer:laptop"), "") << "the laptop's live entry is not back";
  EXPECT_EQ(NameIn(tablet->workflow->DeviceNames(), "peer:laptop"), "");
  for (int i = 0; i < 2; ++i) {
    ASSERT_TRUE(phone->workflow->Publish());
    ASSERT_TRUE(tablet->workflow->Publish());
  }
  EXPECT_EQ(tablet->writes, tablet_writes) << "the tombstone is not rewritten";
  EXPECT_EQ(phone->writes, phone_writes);
  // The tombstone itself is in the locker and on every device, and no one may name the removed device.
  bool tombstone = false;
  for (const DeviceNameEntry& entry : tablet->workflow->DeviceNames()) {
    tombstone = tombstone || (entry.peer_id == "peer:laptop" && entry.removed && entry.name.empty());
  }
  EXPECT_TRUE(tombstone);
  EXPECT_FALSE(tablet->workflow->SetDeviceName("peer:laptop", "Back", util::NowUnixMs() + 1000));
}

TEST_F(AccountLockerWorkflowNamesTest, TheCapIsTheSameOnEveryDeviceWhateverTheirCardsSayAndSettles) {
  std::vector<std::string> all;
  for (int i = 1; i <= 9; ++i) {
    all.push_back("peer:p" + std::to_string(i));
  }
  auto phone = Device("peer:p1", {"peer:p1", "peer:p2"}, "Phone");
  auto laptop = Device("peer:p9", {"peer:p9", "peer:p8", "peer:p7"}, "Laptop");
  const int64_t now = util::NowUnixMs();
  for (int i = 2; i <= 8; ++i) {
    ASSERT_TRUE(
        phone->workflow->SetDeviceName(all[static_cast<size_t>(i - 1)], "Device " + std::to_string(i), now + i));
  }
  ASSERT_TRUE(phone->workflow->Publish()); // its own name (peer:p1, at ~now) plus seven: eight
  ASSERT_TRUE(laptop->workflow->SetDeviceName("peer:p9", "Laptop renamed", now + 100));
  for (int round = 0; round < 2; ++round) {
    ASSERT_TRUE(laptop->workflow->Publish());
    ASSERT_TRUE(phone->workflow->Publish());
  }
  const int phone_writes = phone->writes;
  const int laptop_writes = laptop->writes;
  for (int round = 0; round < 2; ++round) {
    ASSERT_TRUE(laptop->workflow->Publish());
    ASSERT_TRUE(phone->workflow->Publish());
  }
  EXPECT_EQ(phone->writes, phone_writes) << "no more rewriting";
  EXPECT_EQ(laptop->writes, laptop_writes) << "no more rewriting";
  EXPECT_EQ(phone->workflow->DeviceNames(), laptop->workflow->DeviceNames()) << "every device holds the same eight";
  EXPECT_EQ(phone->workflow->DeviceNames().size(), kMaxDeviceNameEntries);
}

TEST_F(AccountLockerWorkflowNamesTest, OnlyTheMainDeviceNamesAnotherAndOnlyOneOnTheList) {
  const auto list = [](const std::string& self, const std::vector<std::string>& peers) {
    OwnDeviceList out;
    out.self_peer_id = self;
    for (const std::string& peer : peers) {
      AccountCardDevice device;
      device.peer_id = peer;
      out.devices.push_back(device);
    }
    return out;
  };
  const OwnDeviceList main_view = list("peer:phone", {"peer:phone", "peer:laptop"});
  EXPECT_TRUE(CanRenameDevice(main_view, "peer:phone"));
  EXPECT_TRUE(CanRenameDevice(main_view, "peer:laptop"));
  EXPECT_FALSE(CanRenameDevice(main_view, "peer:stranger"));
  const OwnDeviceList other_view = list("peer:laptop", {"peer:phone", "peer:laptop"});
  EXPECT_TRUE(CanRenameDevice(other_view, "peer:laptop"));
  EXPECT_FALSE(CanRenameDevice(other_view, "peer:phone")) << "not the main device: only its own name";
  EXPECT_TRUE(CanRenameDevice(list("peer:solo", {}), "peer:solo"));
  OwnDeviceList out = main_view;
  out.signed_out = true;
  EXPECT_FALSE(CanRenameDevice(out, "peer:phone"));
}

TEST_F(AccountLockerWorkflowNamesTest, NamesStayOutOfLogsAndMetrics) {
  auto capture = std::make_shared<Lines>();
  for (const char* channel : {"AccountLocker", "Metrics", "ConversationsHub"}) {
    logging::getLogger(channel).addHandler(capture);
  }
  const std::string marker = "ZZ-NAME-MARKER";
  auto phone = Device("peer:phone", {"peer:phone", "peer:laptop"}, marker + " system");
  ASSERT_TRUE(phone->workflow->Publish());
  ASSERT_TRUE(phone->workflow->SetDeviceName("peer:laptop", marker + " laptop", util::NowUnixMs()));
  ASSERT_TRUE(phone->workflow->Publish());
  auto laptop = Device("peer:laptop", {"peer:phone", "peer:laptop"}, "MacBook");
  ASSERT_TRUE(laptop->workflow->Publish());
  ASSERT_TRUE(phone->workflow->Publish());
  phone->workflow->ForgetDeviceName("peer:laptop", util::NowUnixMs() + 50);
  ASSERT_TRUE(phone->workflow->Publish());
  EXPECT_FALSE(capture->Text().empty()) << "the capture saw the publish lines";
  EXPECT_EQ(capture->Text().find(marker), std::string::npos);
}

TEST_F(AccountLockerWorkflowNamesTest, TheTableSurvivesARestartInAccountLockerJson) {
  workflow_.reset();
  auto phone = Device("peer:phone", {"peer:phone"}, "iPhone");
  phone->workflow->SetDataDir(dir_.string());
  ASSERT_TRUE(phone->workflow->SetDeviceName("peer:phone", "Mine", util::NowUnixMs()));
  auto again = Device("peer:phone", {"peer:phone"}, "iPhone");
  again->workflow->SetDataDir(dir_.string());
  EXPECT_EQ(NameIn(again->workflow->DeviceNames(), "peer:phone"), "Mine");
}

} // namespace
} // namespace pbr
