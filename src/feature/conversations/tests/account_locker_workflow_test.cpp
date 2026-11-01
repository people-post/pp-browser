#include "feature/conversations/AccountLockerWorkflow.h"

#include "foundation/crypto/CryptoConstants.h"
#include "foundation/crypto/RecoverySeed.h"
#include "lib/people/ContactsStore.h"
#include "lib/people/IdentityStore.h"
#include "common/Utilities.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <map>
#include <memory>
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
    registered.relay_user_id = "relay:alice";
    registered.nickname = "alice";
    registered.registered = true;
    ASSERT_TRUE(identity_->Update(registered));
    contacts_ = std::make_unique<ContactsStore>(dir_.string());
    ASSERT_TRUE(contacts_->Upsert(AccountContact("c1", "account:bob", "Bob")));
    workflow_ = std::make_unique<AccountLockerWorkflow>(*identity_, *contacts_, AccountLockerWorkflow::Sources{});
    workflow_->SetClient(&provider_);
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
    restoring.SetClient(&provider_);
    return restoring.Fetch(seed_, saved_copies);
  }

  std::filesystem::path dir_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<AccountLockerWorkflow> workflow_;
  FakeLockerProvider provider_;
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

TEST_F(AccountLockerWorkflowTest, ASavedFileRestoresTheAccountWhenTheProviderHasNone) {
  auto file = workflow_->ExportFileJson();
  ASSERT_TRUE(file) << file.error().message;
  auto restored = Restore({*file});
  ASSERT_TRUE(restored) << restored.error().message;
  ASSERT_TRUE(restored->has_value());
  EXPECT_EQ((*restored)->state.relay_user_id, "relay:alice");
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
  auto new_file = workflow_->ExportFileJson();  // saved after the last write: newer than the provider's
  ASSERT_TRUE(new_file);
  restored = Restore({*new_file});
  ASSERT_TRUE(restored && restored->has_value());
  EXPECT_EQ(BobsName(**restored), "Bob (climbing)") << "the file is newer than the provider's copy";
}

TEST_F(AccountLockerWorkflowTest, AnUnreachableProviderIsNoErrorWithASavedFile) {
  auto file = workflow_->ExportFileJson();
  ASSERT_TRUE(file);
  provider_.unreachable = true;
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

} // namespace
} // namespace pbr
