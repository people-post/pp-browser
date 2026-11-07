#include "foundation/crypto/BiometricUnlock.h"

#include "foundation/crypto/ProfileSecretsEngine.h"

#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pbr {
namespace {

// The secure-blob store in memory, with the biometry level: answers what the next prompt "returns", records prompts.
class BiometryStore final : public ISecureBlobStore {
public:
  bool supported = true;
  bool biometry = true; ///< Supports(kBiometryCurrentSet)
  std::optional<SecureBlobLookup::Status> answer; ///< Set: the next lookups answer this (Unavailable / Canceled)
  int64_t answer_code = 0;
  bool lookup_fails = false;
  bool fail_writes = false;
  std::map<std::string, ByteVector> items;
  std::map<std::string, SecureBlobAccess> levels;
  std::vector<std::string> prompts;
  int reads = 0;
  int deletes = 0;

  bool Supported() const override { return supported; }
  bool Supports(SecureBlobAccess access) const override {
    return supported && (access != SecureBlobAccess::kBiometryCurrentSet || biometry);
  }
  Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes) override {
    return Write(name, bytes, SecureBlobAccess::kAfterFirstUnlock);
  }
  Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes, SecureBlobAccess access) override {
    if (fail_writes) {
      return Error("write refused");
    }
    items[name] = bytes;
    levels[name] = access;
    return {};
  }
  Roe<std::optional<std::vector<uint8_t>>> Read(const std::string& name) override {
    auto it = items.find(name);
    if (it == items.end()) {
      return std::optional<std::vector<uint8_t>>{};
    }
    return std::optional<std::vector<uint8_t>>(it->second);
  }
  Roe<SecureBlobLookup> Lookup(const std::string& name) override {
    ++reads;
    if (lookup_fails) {
      return Error("keychain error");
    }
    SecureBlobLookup out;
    if (answer) {
      out.status = *answer;
      out.code = answer_code;
      return out;
    }
    auto it = items.find(name);
    if (it != items.end()) {
      out.status = SecureBlobLookup::Status::Found;
      out.bytes = it->second;
    }
    return out;
  }
  Roe<SecureBlobLookup> LookupWithPrompt(const std::string& name, const std::string& prompt) override {
    prompts.push_back(prompt);
    return Lookup(name);
  }
  Roe<void> Delete(const std::string& name) override {
    ++deletes;
    items.erase(name);
    levels.erase(name);
    return {};
  }
};

ByteVector Dek(const uint8_t base) {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(base + i);
  }
  return dek;
}

class BiometricUnlockTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_bio_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    bio_ = std::make_unique<BiometricUnlock>(store_, dir_, "p1");
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  bool BothThere() const {
    return store_.items.count(bio_->EntryName()) == 1 && std::filesystem::exists(bio_->FilePath());
  }
  bool NothingLeft() const { return store_.items.empty() && !std::filesystem::exists(bio_->FilePath()); }

  std::filesystem::path dir_;
  BiometryStore store_;
  std::unique_ptr<BiometricUnlock> bio_;
};

TEST_F(BiometricUnlockTest, EnableThenTryUnlockYieldsTheSameDek) {
  ASSERT_TRUE(bio_->Supported());
  EXPECT_FALSE(bio_->Enabled());
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  EXPECT_TRUE(bio_->Enabled());
  EXPECT_EQ(bio_->EntryName(), "p1/unlock_biometric");
  EXPECT_EQ(bio_->FilePath(), dir_ / "biometric.bin");
  const ByteVector& entry = store_.items.at(bio_->EntryName());
  ASSERT_EQ(entry.size(), 33u) << "version 1 + K_bio 32";
  EXPECT_EQ(entry[0], 1);
  EXPECT_EQ(store_.levels.at(bio_->EntryName()), SecureBlobAccess::kBiometryCurrentSet);

  auto got = bio_->TryUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Found);
  EXPECT_EQ(got->dek, Dek(0xA0));
  ASSERT_EQ(store_.prompts.size(), 1u);
  EXPECT_EQ(store_.prompts[0], "Unlock PP");
  EXPECT_TRUE(BothThere()) << "a hit keeps both pieces";
}

TEST_F(BiometricUnlockTest, MissingEntryDeletesTheFile) {
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  store_.items.clear(); // the enrolment changed: the Keychain item is gone for good
  auto got = bio_->TryUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Missing);
  EXPECT_TRUE(got->dek.empty());
  EXPECT_FALSE(bio_->Enabled());
  EXPECT_TRUE(NothingLeft());
}

TEST_F(BiometricUnlockTest, WithoutTheFileThereIsNoPromptAndTheEntryGoes) {
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  std::filesystem::remove(bio_->FilePath());
  auto got = bio_->TryUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Missing);
  EXPECT_EQ(store_.reads, 0) << "no system prompt for a pair that cannot open";
  EXPECT_TRUE(NothingLeft());
}

TEST_F(BiometricUnlockTest, AnotherProfileCannotOpenItAndDeletes) {
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  BiometricUnlock other(store_, dir_, "p2");
  store_.items[other.EntryName()] = store_.items.at(bio_->EntryName());
  store_.items.erase(bio_->EntryName());
  auto got = other.TryUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Missing) << "AAD binds the file to p1";
  EXPECT_TRUE(NothingLeft());
}

TEST_F(BiometricUnlockTest, MalformedEntryIsMissingAndDeletes) {
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  store_.items[bio_->EntryName()][0] = 2; // an unknown version
  auto got = bio_->TryUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Missing);
  EXPECT_TRUE(NothingLeft());
}

TEST_F(BiometricUnlockTest, UnavailableKeepsBoth) {
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  store_.answer = SecureBlobLookup::Status::Unavailable;
  store_.answer_code = -25308; // errSecInteractionNotAllowed: the prompt was interrupted
  auto got = bio_->TryUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Unavailable);
  EXPECT_EQ(got->code, -25308);
  EXPECT_TRUE(got->dek.empty());
  EXPECT_EQ(store_.deletes, 0);
  EXPECT_TRUE(BothThere());
}

TEST_F(BiometricUnlockTest, CanceledKeepsBoth) {
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  for (const int64_t code : {-128, -25293}) { // canceled; the face did not pass
    store_.answer = SecureBlobLookup::Status::Canceled;
    store_.answer_code = code;
    auto got = bio_->TryUnlock("Unlock PP");
    ASSERT_TRUE(static_cast<bool>(got));
    EXPECT_EQ(got->status, BiometricUnlock::Status::Canceled);
    EXPECT_EQ(got->code, code);
    EXPECT_TRUE(got->dek.empty());
  }
  EXPECT_EQ(store_.deletes, 0);
  EXPECT_TRUE(BothThere());
}

TEST_F(BiometricUnlockTest, StoreErrorIsReturnedAndKeepsBoth) {
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  store_.lookup_fails = true;
  EXPECT_FALSE(static_cast<bool>(bio_->TryUnlock("Unlock PP")));
  EXPECT_EQ(store_.deletes, 0);
  EXPECT_TRUE(BothThere());
}

TEST_F(BiometricUnlockTest, DisableDeletesBoth) {
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  bio_->Disable();
  EXPECT_FALSE(bio_->Enabled());
  EXPECT_TRUE(NothingLeft());
  EXPECT_EQ(store_.reads, 0) << "turning it off never prompts";
}

TEST_F(BiometricUnlockTest, EnableReplacesAnOlderPair) {
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  const ByteVector first_entry = store_.items.at(bio_->EntryName());
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0x10))));
  EXPECT_NE(store_.items.at(bio_->EntryName()), first_entry) << "a fresh K_bio";
  auto got = bio_->TryUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->dek, Dek(0x10));
}

// BiometricUnlock does not judge the DEK: a pair sealed over another DEK is still Found (the engine decides).
TEST_F(BiometricUnlockTest, AWrongDekIsStillFound) {
  ASSERT_TRUE(static_cast<bool>(bio_->Enable(Dek(0x55))));
  auto got = bio_->TryUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Found);
  EXPECT_EQ(got->dek, Dek(0x55));
}

TEST_F(BiometricUnlockTest, StoreWriteFailureLeavesNothingBehind) {
  store_.fail_writes = true;
  EXPECT_FALSE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  EXPECT_TRUE(NothingLeft());
}

TEST_F(BiometricUnlockTest, WithoutTheBiometryLevelNothingIsReadOrWritten) {
  store_.biometry = false;
  EXPECT_FALSE(bio_->Supported());
  EXPECT_FALSE(static_cast<bool>(bio_->Enable(Dek(0xA0))));
  auto got = bio_->TryUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Missing);
  EXPECT_EQ(store_.reads, 0);
  EXPECT_TRUE(NothingLeft());
}

// The default store refuses the level instead of keeping a "biometric" key unprotected.
TEST(SecureBlobStoreDefaultsTest, TheBiometryLevelIsNotSupportedByDefault) {
  class PlainStore final : public ISecureBlobStore {
  public:
    bool Supported() const override { return true; }
    Roe<void> Write(const std::string&, const std::vector<uint8_t>&) override {
      ++writes;
      return {};
    }
    Roe<std::optional<std::vector<uint8_t>>> Read(const std::string&) override {
      return std::optional<std::vector<uint8_t>>{};
    }
    Roe<void> Delete(const std::string&) override { return {}; }
    int writes = 0;
  };
  PlainStore store;
  EXPECT_TRUE(store.Supports(SecureBlobAccess::kWhenUnlocked));
  EXPECT_FALSE(store.Supports(SecureBlobAccess::kBiometryCurrentSet));
  ISecureBlobStore& base = store;
  EXPECT_FALSE(static_cast<bool>(base.Write("x", {1}, SecureBlobAccess::kBiometryCurrentSet)));
  EXPECT_TRUE(static_cast<bool>(base.Write("x", {1}, SecureBlobAccess::kWhenUnlocked)));
  EXPECT_EQ(store.writes, 1);
}

// --- ProfileSecretsEngine with a real vault on disk. ---

class ProfileSecretsEngineBiometricTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_bio_engine_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    ASSERT_TRUE(static_cast<bool>(engine_.Initialize(dir_.string())));
    ASSERT_TRUE(static_cast<bool>(engine_.Unlock(SecretText("1234"))));
  }
  void TearDown() override {
    engine_.Shutdown();
    std::filesystem::remove_all(dir_);
  }
  void Attach() { engine_.AttachBiometric(std::make_unique<BiometricUnlock>(store_, dir_, engine_.ProfileId())); }
  BiometricUnlock Probe() { return BiometricUnlock(store_, dir_, engine_.ProfileId()); }

  std::filesystem::path dir_;
  BiometryStore store_;
  ProfileSecretsEngine engine_;
};

TEST_F(ProfileSecretsEngineBiometricTest, WithoutBiometricEverythingIsANoOp) {
  EXPECT_FALSE(engine_.BiometricSupported());
  EXPECT_FALSE(engine_.BiometricEnabled());
  EXPECT_FALSE(static_cast<bool>(engine_.EnableBiometric("1234")));
  engine_.DisableBiometric();
  engine_.Lock();
  auto got = engine_.TryBiometricUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Missing);
  EXPECT_FALSE(engine_.IsUnlocked());
}

TEST_F(ProfileSecretsEngineBiometricTest, EnableNeedsTheRightPin) {
  Attach();
  ASSERT_TRUE(engine_.BiometricSupported());
  EXPECT_FALSE(static_cast<bool>(engine_.EnableBiometric("0000")));
  EXPECT_FALSE(engine_.BiometricEnabled());
  EXPECT_TRUE(store_.items.empty());
  ASSERT_TRUE(static_cast<bool>(engine_.EnableBiometric("1234")));
  EXPECT_TRUE(engine_.BiometricEnabled());
}

TEST_F(ProfileSecretsEngineBiometricTest, EnableWorksWhileLockedToo) {
  Attach();
  engine_.Lock();
  ASSERT_TRUE(static_cast<bool>(engine_.EnableBiometric("1234")));
  EXPECT_FALSE(engine_.IsUnlocked()) << "enabling opens the vault on the side only";
}

TEST_F(ProfileSecretsEngineBiometricTest, BiometricUnlocksTheVaultAndFansOut) {
  Attach();
  ASSERT_TRUE(static_cast<bool>(engine_.EnableBiometric("1234")));
  auto dek = engine_.Vault()->Dek();
  ASSERT_TRUE(static_cast<bool>(dek));
  const ByteVector expected = *dek;

  engine_.Lock();
  int unlocked_calls = 0;
  engine_.SetOnUnlocked([&]() { ++unlocked_calls; });
  auto got = engine_.TryBiometricUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Found);
  EXPECT_TRUE(got->dek.empty()) << "the plaintext DEK does not leave the engine";
  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_EQ(unlocked_calls, 1);
  ASSERT_EQ(store_.prompts.size(), 1u);
  EXPECT_EQ(store_.prompts[0], "Unlock PP");
  auto after = engine_.Vault()->Dek();
  ASSERT_TRUE(static_cast<bool>(after));
  EXPECT_EQ(*after, expected);
}

TEST_F(ProfileSecretsEngineBiometricTest, CanceledStaysLockedAndKeepsBoth) {
  Attach();
  ASSERT_TRUE(static_cast<bool>(engine_.EnableBiometric("1234")));
  engine_.Lock();
  store_.answer = SecureBlobLookup::Status::Canceled;
  store_.answer_code = -128;
  auto got = engine_.TryBiometricUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Canceled);
  EXPECT_EQ(got->code, -128);
  EXPECT_FALSE(engine_.IsUnlocked());
  EXPECT_TRUE(engine_.BiometricEnabled());
}

TEST_F(ProfileSecretsEngineBiometricTest, MissingEntryTurnsItOff) {
  Attach();
  ASSERT_TRUE(static_cast<bool>(engine_.EnableBiometric("1234")));
  engine_.Lock();
  store_.items.clear();
  auto got = engine_.TryBiometricUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Missing);
  EXPECT_FALSE(engine_.IsUnlocked());
  EXPECT_FALSE(engine_.BiometricEnabled());
}

// A DEK with no vault to take it is Missing: both pieces go.
TEST_F(ProfileSecretsEngineBiometricTest, FoundWithoutAVaultIsMissingAndDeletesBoth) {
  Attach();
  ASSERT_TRUE(static_cast<bool>(engine_.EnableBiometric("1234")));
  engine_.Lock();
  std::filesystem::remove(DataKeyVault::VaultPathForProfile(dir_.string()));
  auto got = engine_.TryBiometricUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Missing);
  EXPECT_TRUE(got->dek.empty());
  EXPECT_FALSE(engine_.IsUnlocked());
  EXPECT_TRUE(store_.items.empty());
  EXPECT_FALSE(std::filesystem::exists(Probe().FilePath()));
}

TEST_F(ProfileSecretsEngineBiometricTest, AlreadyUnlockedLeavesTheStateAlone) {
  Attach();
  ASSERT_TRUE(static_cast<bool>(engine_.EnableBiometric("1234")));
  int unlocked_calls = 0;
  engine_.SetOnUnlocked([&]() { ++unlocked_calls; });
  auto got = engine_.TryBiometricUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Found);
  EXPECT_TRUE(got->dek.empty());
  EXPECT_EQ(unlocked_calls, 0);
}

TEST_F(ProfileSecretsEngineBiometricTest, ChangePinKeepsItAndDisableDeletesBoth) {
  Attach();
  ASSERT_TRUE(static_cast<bool>(engine_.EnableBiometric("1234")));
  ASSERT_TRUE(static_cast<bool>(engine_.ChangePin("1234", "5678")));
  EXPECT_TRUE(engine_.BiometricEnabled()) << "the DEK did not change";
  engine_.Lock();
  auto got = engine_.TryBiometricUnlock("Unlock PP");
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, BiometricUnlock::Status::Found);
  EXPECT_TRUE(engine_.IsUnlocked());

  engine_.DisableBiometric();
  EXPECT_FALSE(engine_.BiometricEnabled());
  EXPECT_TRUE(store_.items.empty());
  EXPECT_FALSE(std::filesystem::exists(Probe().FilePath()));
}

} // namespace
} // namespace pbr
