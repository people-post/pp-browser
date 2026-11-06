#include "foundation/crypto/GraceUnlock.h"

#include "foundation/crypto/ProfileSecretsEngine.h"

#include <algorithm>
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

// The secure-blob store in memory: tells Missing from Unavailable, can refuse writes, counts every touch.
class MemoryStore final : public ISecureBlobStore {
public:
  bool supported = true;
  bool unavailable = false; ///< Lookup answers Unavailable (device locked)
  int64_t unavailable_code = -25308;
  bool lookup_fails = false; ///< Lookup returns an error
  bool fail_writes = false;
  std::map<std::string, ByteVector> items;
  std::map<std::string, SecureBlobAccess> levels;
  int reads = 0;
  int writes = 0;
  int deletes = 0;

  bool Supported() const override { return supported; }
  Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes) override {
    return Write(name, bytes, SecureBlobAccess::kAfterFirstUnlock);
  }
  Roe<void> Write(const std::string& name, const std::vector<uint8_t>& bytes, SecureBlobAccess access) override {
    ++writes;
    if (fail_writes) {
      return Error("write refused");
    }
    items[name] = bytes;
    levels[name] = access;
    return {};
  }
  Roe<std::optional<std::vector<uint8_t>>> Read(const std::string& name) override {
    ++reads;
    if (unavailable || lookup_fails) {
      return Error("unavailable");
    }
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
    if (unavailable) {
      out.status = SecureBlobLookup::Status::Unavailable;
      out.code = unavailable_code;
      return out;
    }
    auto it = items.find(name);
    if (it != items.end()) {
      out.status = SecureBlobLookup::Status::Found;
      out.bytes = it->second;
    }
    return out;
  }
  Roe<void> Delete(const std::string& name) override {
    ++deletes;
    items.erase(name);
    levels.erase(name);
    return {};
  }
};

GraceClock Clock(const int64_t mono, const uint8_t boot = 7) {
  GraceClock clock;
  clock.boot_id.fill(boot);
  clock.mono_seconds = mono;
  return clock;
}

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xA0 + i);
  }
  return dek;
}

ByteVector ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return ByteVector(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

int64_t EntryExpiry(const ByteVector& entry) {
  uint64_t value = 0;
  for (size_t i = 0; i < 8; ++i) {
    value |= static_cast<uint64_t>(entry[49 + i]) << (8 * i);
  }
  return static_cast<int64_t>(value);
}

class GraceUnlockTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_grace_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    grace_ = std::make_unique<GraceUnlock>(store_, dir_, "p1");
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  bool NothingLeft() const { return store_.items.empty() && !std::filesystem::exists(grace_->FilePath()); }
  bool BothPresent() const {
    return store_.items.count(grace_->EntryName()) == 1 && std::filesystem::exists(grace_->FilePath());
  }
  GraceUnlock::Status Try(const GraceClock& now) {
    auto got = grace_->TryUnlock(now);
    EXPECT_TRUE(static_cast<bool>(got));
    return got ? got->status : GraceUnlock::Status::Missing;
  }

  std::filesystem::path dir_;
  MemoryStore store_;
  std::unique_ptr<GraceUnlock> grace_;
};

TEST_F(GraceUnlockTest, RememberThenTryUnlockReturnsTheSameDek) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  ASSERT_TRUE(BothPresent());
  EXPECT_EQ(grace_->EntryName(), "p1/unlock_grace");
  EXPECT_EQ(grace_->FilePath(), dir_ / "grace.bin");
  const ByteVector& entry = store_.items.at(grace_->EntryName());
  EXPECT_EQ(entry.size(), 57u);
  EXPECT_EQ(entry[0], 1);
  EXPECT_EQ(store_.levels.at(grace_->EntryName()), SecureBlobAccess::kWhenUnlocked);
  // Neither piece holds the DEK in the clear.
  const ByteVector dek = TestDek();
  for (const ByteVector& piece : {entry, ReadFile(grace_->FilePath())}) {
    EXPECT_EQ(std::search(piece.begin(), piece.end(), dek.begin(), dek.end()), piece.end());
  }

  auto got = grace_->TryUnlock(Clock(1600));
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, GraceUnlock::Status::Found);
  EXPECT_EQ(got->dek, TestDek());
  EXPECT_TRUE(BothPresent());
}

TEST_F(GraceUnlockTest, ExpiresAtTheExactSecondAndDeletesBoth) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  EXPECT_EQ(Try(Clock(1899)), GraceUnlock::Status::Found);
  EXPECT_EQ(Try(Clock(1900)), GraceUnlock::Status::Expired);
  EXPECT_TRUE(NothingLeft());

  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  EXPECT_EQ(Try(Clock(5000)), GraceUnlock::Status::Expired);
  EXPECT_TRUE(NothingLeft());
}

TEST_F(GraceUnlockTest, AnotherBootIsExpiredAndDeletesBoth) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000, 7))));
  // After a reboot the monotonic clock restarts low: still expired, because the boot differs.
  EXPECT_EQ(Try(Clock(10, 8)), GraceUnlock::Status::Expired);
  EXPECT_TRUE(NothingLeft());
}

TEST_F(GraceUnlockTest, OnlyTheMonotonicClockCounts) {
  // GraceClock carries no wall time: the end is stored on the monotonic scale, so moving the system clock
  // (which leaves boot_id and mono_seconds alone) cannot reopen or close the window.
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  EXPECT_EQ(EntryExpiry(store_.items.at(grace_->EntryName())), 1900);
  EXPECT_EQ(Try(Clock(1000)), GraceUnlock::Status::Found);
}

TEST_F(GraceUnlockTest, AnotherProfileCannotOpenItAndDeletes) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  GraceUnlock other(store_, dir_, "p2");
  store_.items[other.EntryName()] = store_.items.at(grace_->EntryName()); // same key, other AAD
  auto got = other.TryUnlock(Clock(1100));
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, GraceUnlock::Status::Missing);
  EXPECT_TRUE(got->dek.empty());
  EXPECT_EQ(store_.items.count(other.EntryName()), 0u);
  EXPECT_FALSE(std::filesystem::exists(other.FilePath()));
}

TEST_F(GraceUnlockTest, OnlyOnePieceIsMissingAndDeletesTheOther) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  std::filesystem::remove(grace_->FilePath());
  EXPECT_EQ(Try(Clock(1100)), GraceUnlock::Status::Missing);
  EXPECT_TRUE(NothingLeft());

  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  store_.items.clear();
  EXPECT_EQ(Try(Clock(1100)), GraceUnlock::Status::Missing);
  EXPECT_TRUE(NothingLeft());
}

TEST_F(GraceUnlockTest, MalformedEntryIsMissingAndDeletes) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  store_.items.at(grace_->EntryName())[0] = 2; // unknown version
  EXPECT_EQ(Try(Clock(1100)), GraceUnlock::Status::Missing);
  EXPECT_TRUE(NothingLeft());

  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  store_.items.at(grace_->EntryName()).pop_back(); // wrong length
  EXPECT_EQ(Try(Clock(1100)), GraceUnlock::Status::Missing);
  EXPECT_TRUE(NothingLeft());

  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  store_.items.at(grace_->EntryName())[5] ^= 0x01; // another K_grace: the file does not decrypt
  EXPECT_EQ(Try(Clock(1100)), GraceUnlock::Status::Missing);
  EXPECT_TRUE(NothingLeft());
}

TEST_F(GraceUnlockTest, ZeroMinutesWritesNothingAndForgets) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 0, Clock(1000))));
  EXPECT_EQ(store_.writes, 0);
  EXPECT_TRUE(NothingLeft());

  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 0, Clock(1100))));
  EXPECT_TRUE(NothingLeft());
}

// GraceClock: mono 0 is "no clock". Nothing opens, nothing is written, an existing entry is left alone.
TEST_F(GraceUnlockTest, NoClockFailsClosedAndTouchesNothing) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  const ByteVector entry = store_.items.at(grace_->EntryName());
  const ByteVector file = ReadFile(grace_->FilePath());
  const int reads = store_.reads;
  const int writes = store_.writes;
  const int deletes = store_.deletes;

  auto got = grace_->TryUnlock(Clock(0));
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, GraceUnlock::Status::Missing);
  EXPECT_TRUE(got->dek.empty());
  EXPECT_TRUE(static_cast<bool>(grace_->Touch(15, Clock(0))));
  EXPECT_EQ(store_.reads, reads);
  EXPECT_EQ(store_.writes, writes);
  EXPECT_EQ(store_.deletes, deletes);
  EXPECT_EQ(store_.items.at(grace_->EntryName()), entry);
  EXPECT_EQ(ReadFile(grace_->FilePath()), file);
  EXPECT_EQ(Try(Clock(1100)), GraceUnlock::Status::Found); // still the same window once the clock is back
}

TEST_F(GraceUnlockTest, RememberWithoutAClockWritesNothing) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(0))));
  EXPECT_EQ(store_.writes, 0);
  EXPECT_TRUE(NothingLeft());
}

TEST_F(GraceUnlockTest, TouchMovesTheEnd) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  ASSERT_TRUE(static_cast<bool>(grace_->Touch(15, Clock(1600)))); // new end 2500
  EXPECT_EQ(Try(Clock(1950)), GraceUnlock::Status::Found);        // past the old end 1900
  EXPECT_EQ(Try(Clock(2500)), GraceUnlock::Status::Expired);
}

TEST_F(GraceUnlockTest, TouchCanShortenTheWindow) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  ASSERT_TRUE(static_cast<bool>(grace_->Touch(5, Clock(1000)))); // 15 → 5 minutes: end 1300
  EXPECT_EQ(Try(Clock(1299)), GraceUnlock::Status::Found);
  EXPECT_EQ(Try(Clock(1300)), GraceUnlock::Status::Expired);
}

TEST_F(GraceUnlockTest, TouchAfterForgetDoesNotResurrect) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  grace_->Forget();
  EXPECT_TRUE(NothingLeft());
  const int writes = store_.writes;
  ASSERT_TRUE(static_cast<bool>(grace_->Touch(15, Clock(1100))));
  EXPECT_EQ(store_.writes, writes);
  EXPECT_TRUE(NothingLeft());
  EXPECT_EQ(Try(Clock(1200)), GraceUnlock::Status::Missing);
}

// Touch runs only in the foreground: the user is here, so an entry that has run out is renewed from now.
TEST_F(GraceUnlockTest, TouchRenewsAnExpiredEntry) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000, 7))));
  ASSERT_TRUE(static_cast<bool>(grace_->Touch(15, Clock(5000, 7)))); // long past its end 1900
  EXPECT_EQ(EntryExpiry(store_.items.at(grace_->EntryName())), 5900);
  EXPECT_EQ(Try(Clock(5899, 7)), GraceUnlock::Status::Found);
  EXPECT_EQ(Try(Clock(5900, 7)), GraceUnlock::Status::Expired);
}

TEST_F(GraceUnlockTest, TouchAfterAnotherBootRenewsToo) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000, 7))));
  ASSERT_TRUE(static_cast<bool>(grace_->Touch(15, Clock(10, 8)))); // rebooted, app back in the foreground
  const ByteVector& entry = store_.items.at(grace_->EntryName());
  EXPECT_EQ(entry[1 + 32], 8); // boot_id rewritten from the clock
  EXPECT_EQ(EntryExpiry(entry), 910);
  auto got = grace_->TryUnlock(Clock(500, 8));
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, GraceUnlock::Status::Found);
  EXPECT_EQ(got->dek, TestDek());
  EXPECT_EQ(Try(Clock(500, 7)), GraceUnlock::Status::Expired); // the old boot no longer matches
}

TEST_F(GraceUnlockTest, TouchRewritesOnlyTheStoreEntry) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  const ByteVector file_before = ReadFile(grace_->FilePath());
  const ByteVector entry_before = store_.items.at(grace_->EntryName());
  ASSERT_TRUE(static_cast<bool>(grace_->Touch(15, Clock(1300))));
  EXPECT_EQ(ReadFile(grace_->FilePath()), file_before);
  const ByteVector& entry_after = store_.items.at(grace_->EntryName());
  EXPECT_NE(entry_after, entry_before);
  EXPECT_TRUE(std::equal(entry_before.begin(), entry_before.begin() + 49, entry_after.begin())); // same K, boot
  EXPECT_EQ(EntryExpiry(entry_after), 2200);
  EXPECT_EQ(store_.levels.at(grace_->EntryName()), SecureBlobAccess::kWhenUnlocked);
}

TEST_F(GraceUnlockTest, UnavailableDeletesNothingAndTouchSkips) {
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  const ByteVector entry = store_.items.at(grace_->EntryName());
  const int writes = store_.writes;

  store_.unavailable = true;
  for (const int64_t code : {int64_t{-25308}, int64_t{-34018}, int64_t{12345}}) {
    store_.unavailable_code = code;
    EXPECT_EQ(Try(Clock(1100)), GraceUnlock::Status::Unavailable);
    EXPECT_TRUE(BothPresent());
  }
  // Even past the end: the entry cannot be read, so it cannot be judged; it stays.
  EXPECT_EQ(Try(Clock(9999)), GraceUnlock::Status::Unavailable);
  EXPECT_TRUE(static_cast<bool>(grace_->Touch(15, Clock(1100))));
  EXPECT_EQ(store_.writes, writes);
  EXPECT_EQ(store_.deletes, 0);

  store_.unavailable = false;
  store_.lookup_fails = true; // an error from the store itself is no "Missing" either
  EXPECT_EQ(Try(Clock(1100)), GraceUnlock::Status::Unavailable);
  EXPECT_TRUE(static_cast<bool>(grace_->Touch(15, Clock(1100))));
  EXPECT_EQ(store_.deletes, 0);
  EXPECT_TRUE(BothPresent());

  store_.lookup_fails = false;
  EXPECT_EQ(store_.items.at(grace_->EntryName()), entry);
  EXPECT_EQ(Try(Clock(1100)), GraceUnlock::Status::Found);
}

TEST_F(GraceUnlockTest, UnsupportedStoreReadsAndWritesNothing) {
  store_.supported = false;
  EXPECT_FALSE(grace_->Supported());
  EXPECT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  EXPECT_FALSE(std::filesystem::exists(grace_->FilePath()));
  EXPECT_TRUE(static_cast<bool>(grace_->Touch(15, Clock(1000))));
  EXPECT_EQ(Try(Clock(1000)), GraceUnlock::Status::Missing);

  // A grace.bin that is somehow there is not touched either.
  {
    std::ofstream(grace_->FilePath(), std::ios::binary) << "x";
  }
  grace_->Forget();
  EXPECT_EQ(Try(Clock(1000)), GraceUnlock::Status::Missing);
  EXPECT_TRUE(std::filesystem::exists(grace_->FilePath()));
  EXPECT_EQ(store_.reads, 0);
  EXPECT_EQ(store_.writes, 0);
  EXPECT_EQ(store_.deletes, 0);
}

TEST_F(GraceUnlockTest, StoreWriteFailureLeavesNothingBehind) {
  store_.fail_writes = true;
  EXPECT_FALSE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  EXPECT_TRUE(NothingLeft());

  // A later Remember that fails takes the older window with it: no file paired with a stale key.
  store_.fail_writes = false;
  ASSERT_TRUE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1000))));
  store_.fail_writes = true;
  EXPECT_FALSE(static_cast<bool>(grace_->Remember(TestDek(), 15, Clock(1100))));
  EXPECT_TRUE(NothingLeft());
}

// --- ProfileSecretsEngine with a real vault on disk. ---

class ProfileSecretsEngineGraceTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() /
           ("pp_grace_engine_" + std::to_string(reinterpret_cast<uintptr_t>(this)));
    std::filesystem::remove_all(dir_);
    std::filesystem::create_directories(dir_);
    ASSERT_TRUE(static_cast<bool>(engine_.Initialize(dir_.string())));
    ASSERT_TRUE(static_cast<bool>(engine_.Unlock(SecretText("1234"))));
  }
  void TearDown() override {
    engine_.Shutdown();
    std::filesystem::remove_all(dir_);
  }
  void Attach() {
    engine_.AttachGrace(std::make_unique<GraceUnlock>(store_, dir_, engine_.ProfileId()),
                        [this]() { return Clock(now_); });
  }

  std::filesystem::path dir_;
  MemoryStore store_;
  int64_t now_ = 1000;
  ProfileSecretsEngine engine_;
};

TEST_F(ProfileSecretsEngineGraceTest, WithoutGraceEverythingIsANoOp) {
  EXPECT_FALSE(engine_.GraceSupported());
  EXPECT_TRUE(static_cast<bool>(engine_.RememberGrace(15)));
  EXPECT_TRUE(static_cast<bool>(engine_.TouchGrace(15)));
  engine_.ForgetGrace();
  engine_.Lock();
  auto got = engine_.TryGraceUnlock();
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, GraceUnlock::Status::Missing);
  EXPECT_FALSE(engine_.IsUnlocked());
}

TEST_F(ProfileSecretsEngineGraceTest, GraceUnlocksTheVaultAndFansOut) {
  Attach();
  ASSERT_TRUE(engine_.GraceSupported());
  ASSERT_TRUE(static_cast<bool>(engine_.RememberGrace(15)));
  auto dek = engine_.Vault()->Dek();
  ASSERT_TRUE(static_cast<bool>(dek));
  const ByteVector expected = *dek;

  engine_.Lock();
  int unlocked_calls = 0;
  engine_.SetOnUnlocked([&]() { ++unlocked_calls; });
  now_ = 1500;
  auto got = engine_.TryGraceUnlock();
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, GraceUnlock::Status::Found);
  EXPECT_TRUE(got->dek.empty()) << "the plaintext DEK does not leave the engine";
  EXPECT_TRUE(engine_.IsUnlocked());
  EXPECT_EQ(unlocked_calls, 1);
  auto after = engine_.Vault()->Dek();
  ASSERT_TRUE(static_cast<bool>(after));
  EXPECT_EQ(*after, expected);
}

TEST_F(ProfileSecretsEngineGraceTest, RememberNeedsAnUnlockedVault) {
  Attach();
  engine_.Lock();
  EXPECT_FALSE(static_cast<bool>(engine_.RememberGrace(15)));
  EXPECT_TRUE(store_.items.empty());
}

TEST_F(ProfileSecretsEngineGraceTest, ProfileSecretsEngineForgetsGraceWhenPinChanges) {
  Attach();
  ASSERT_TRUE(static_cast<bool>(engine_.RememberGrace(15)));
  GraceUnlock probe(store_, dir_, engine_.ProfileId());

  // Wrong current PIN: nothing changes, the window stays.
  EXPECT_FALSE(static_cast<bool>(engine_.ChangePin("0000", "5678")));
  EXPECT_EQ(store_.items.count(probe.EntryName()), 1u);
  EXPECT_TRUE(std::filesystem::exists(probe.FilePath()));

  ASSERT_TRUE(static_cast<bool>(engine_.ChangePin("1234", "5678")));
  auto got = probe.TryUnlock(Clock(1100));
  ASSERT_TRUE(static_cast<bool>(got));
  EXPECT_EQ(got->status, GraceUnlock::Status::Missing);
  EXPECT_TRUE(store_.items.empty());
  EXPECT_FALSE(std::filesystem::exists(probe.FilePath()));
}

} // namespace
} // namespace pbr
