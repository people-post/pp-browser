// B183: what renewing the Brief LLM key after an HTTP 401 does to the stored identity.
#include "domain/registration/BriefLlmKeyRenewal.h"

#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

#include <functional>
#include <string>
#include <vector>

namespace pbr {
namespace {

class BriefLlmKeyRenewalTest : public ::testing::Test {
protected:
  BriefLlmKeyStore Store() {
    BriefLlmKeyStore store;
    store.get = [this]() -> std::optional<LocalIdentity> {
      ++gets_;
      if (!readable_) {
        return std::nullopt;
      }
      return identity_;
    };
    store.modify = [this](const std::function<bool(LocalIdentity&)>& change) -> Roe<void> {
      if (!readable_) {
        return AppError::Internal("locked");
      }
      LocalIdentity changed = identity_;
      if (!change(changed)) {
        return {};
      }
      ++saves_;
      if (!save_ok_) {
        return AppError::Internal("disk full");
      }
      identity_ = changed;
      return {};
    };
    store.mint = [this](const std::string& base_url) -> Roe<BriefGuestLlmMintResult> {
      mint_urls_.push_back(base_url);
      if (during_mint_) {
        during_mint_(identity_); // another writer while the mint is out
      }
      if (!mint_ok_) {
        return AppError::Network(Err::Network::HttpError, "Guest LLM mint failed (HTTP 503)");
      }
      BriefGuestLlmMintResult out;
      out.llm_api_key = "brf_guest_new";
      return out;
    };
    return store;
  }

  LocalIdentity identity_;
  std::function<void(LocalIdentity&)> during_mint_;
  bool readable_ = true;
  bool save_ok_ = true;
  bool mint_ok_ = true;
  int gets_ = 0;
  int saves_ = 0;
  std::vector<std::string> mint_urls_;
};

TEST_F(BriefLlmKeyRenewalTest, ARefusedRegisteredKeyStaysUntilTheRetryIsAnswered) {
  identity_.brief_llm_api_key = "brf_llm_old";
  auto renewed = RenewBriefLlmKey(Store(), "brf_llm_old", "https://x/api/llm/v1");
  ASSERT_TRUE(renewed) << renewed.error().message;
  EXPECT_EQ(renewed->key, "brf_guest_new");
  EXPECT_FALSE(renewed->reused);
  EXPECT_EQ(mint_urls_, std::vector<std::string>{"https://x/api/llm/v1"});
  EXPECT_EQ(saves_, 1); // one write, after the mint
  EXPECT_EQ(identity_.brief_llm_api_key, "brf_llm_old");
  EXPECT_EQ(identity_.brief_llm_guest_api_key, "brf_guest_new");
  EXPECT_EQ(StoredBriefLlmKey(Store()), "brf_llm_old"); // still what the next turn would send until committed

  ASSERT_TRUE(CommitBriefLlmKey(Store(), "brf_llm_old"));
  EXPECT_TRUE(identity_.brief_llm_api_key.empty());
  EXPECT_EQ(identity_.brief_llm_guest_api_key, "brf_guest_new");
  EXPECT_EQ(StoredBriefLlmKey(Store()), "brf_guest_new");
  EXPECT_EQ(saves_, 2);
}

TEST_F(BriefLlmKeyRenewalTest, ARefusedGuestKeyIsReplaced) {
  identity_.brief_llm_guest_api_key = "brf_guest_old";
  auto renewed = RenewBriefLlmKey(Store(), "brf_guest_old", "u");
  ASSERT_TRUE(renewed);
  EXPECT_EQ(renewed->key, "brf_guest_new");
  EXPECT_EQ(identity_.brief_llm_guest_api_key, "brf_guest_new");
  EXPECT_TRUE(identity_.brief_llm_api_key.empty());
  EXPECT_EQ(saves_, 1);

  ASSERT_TRUE(CommitBriefLlmKey(Store(), "brf_guest_old"));
  EXPECT_EQ(saves_, 1); // nothing left to drop
  EXPECT_EQ(identity_.brief_llm_guest_api_key, "brf_guest_new");
}

TEST_F(BriefLlmKeyRenewalTest, AKeyThatChangedMeanwhileIsReusedWithoutAMint) {
  identity_.brief_llm_guest_api_key = "brf_guest_other";
  auto renewed = RenewBriefLlmKey(Store(), "brf_llm_old", "u");
  ASSERT_TRUE(renewed);
  EXPECT_EQ(renewed->key, "brf_guest_other");
  EXPECT_TRUE(renewed->reused);
  EXPECT_TRUE(mint_urls_.empty());
  EXPECT_EQ(saves_, 0);
}

TEST_F(BriefLlmKeyRenewalTest, AFailedMintChangesNothing) {
  identity_.brief_llm_api_key = "brf_llm_old";
  mint_ok_ = false;
  EXPECT_FALSE(RenewBriefLlmKey(Store(), "brf_llm_old", "u"));
  EXPECT_EQ(saves_, 0);
  EXPECT_EQ(identity_.brief_llm_api_key, "brf_llm_old");
  EXPECT_TRUE(identity_.brief_llm_guest_api_key.empty());
}

TEST_F(BriefLlmKeyRenewalTest, AFailedSaveIsAnErrorAndKeepsTheRegisteredKey) {
  identity_.brief_llm_api_key = "brf_llm_old";
  save_ok_ = false;
  EXPECT_FALSE(RenewBriefLlmKey(Store(), "brf_llm_old", "u"));
  EXPECT_EQ(identity_.brief_llm_api_key, "brf_llm_old");
  EXPECT_FALSE(CommitBriefLlmKey(Store(), "brf_llm_old"));
  EXPECT_EQ(identity_.brief_llm_api_key, "brf_llm_old");
}

TEST_F(BriefLlmKeyRenewalTest, CommitLeavesARegisteredKeyThatIsNotTheRefusedOne) {
  identity_.brief_llm_api_key = "brf_llm_renewed";
  ASSERT_TRUE(CommitBriefLlmKey(Store(), "brf_llm_old"));
  EXPECT_EQ(identity_.brief_llm_api_key, "brf_llm_renewed");
  EXPECT_EQ(saves_, 0);
}

TEST_F(BriefLlmKeyRenewalTest, AnUnreadableIdentityIsAnError) {
  readable_ = false;
  EXPECT_FALSE(RenewBriefLlmKey(Store(), "k", "u"));
  EXPECT_FALSE(CommitBriefLlmKey(Store(), "k"));
  EXPECT_EQ(StoredBriefLlmKey(Store()), "");
  EXPECT_TRUE(mint_urls_.empty());
}

// A key stored while the mint was out (the unlock's guest-key mint, another turn) stays: no overwrite, it is reused.
TEST_F(BriefLlmKeyRenewalTest, AKeyStoredDuringTheMintIsKeptAndReused) {
  identity_.brief_llm_api_key = "brf_llm_old";
  during_mint_ = [](LocalIdentity& identity) { identity.brief_llm_guest_api_key = "brf_guest_meanwhile"; };
  auto renewed = RenewBriefLlmKey(Store(), "brf_llm_old", "u");
  ASSERT_TRUE(renewed) << renewed.error().message;
  EXPECT_TRUE(renewed->reused);
  EXPECT_EQ(renewed->key, "brf_guest_meanwhile");
  EXPECT_EQ(identity_.brief_llm_guest_api_key, "brf_guest_meanwhile");
  EXPECT_EQ(saves_, 0);
}

// The profile changed while the mint was out: the key is not written into the other account.
TEST_F(BriefLlmKeyRenewalTest, AnotherAccountStoredDuringTheMintIsRefused) {
  identity_.account_id = "account:a";
  identity_.brief_llm_guest_api_key = "brf_guest_old";
  during_mint_ = [](LocalIdentity& identity) {
    identity = LocalIdentity{};
    identity.account_id = "account:b";
  };
  EXPECT_FALSE(RenewBriefLlmKey(Store(), "brf_guest_old", "u"));
  EXPECT_EQ(saves_, 0);
  EXPECT_TRUE(identity_.brief_llm_guest_api_key.empty());
}

TEST_F(BriefLlmKeyRenewalTest, StoreGuestKeyIfNoneStoresKeepsOrRefuses) {
  identity_.account_id = "account:a";
  auto stored = StoreBriefGuestKeyIfNone(Store(), SecretText("brf_guest_new"), "account:a");
  ASSERT_TRUE(stored) << stored.error().message;
  EXPECT_EQ(*stored, "brf_guest_new");
  EXPECT_EQ(identity_.brief_llm_guest_api_key, "brf_guest_new");
  EXPECT_EQ(saves_, 1);

  stored = StoreBriefGuestKeyIfNone(Store(), SecretText("brf_guest_second"), "account:a");
  ASSERT_TRUE(stored);
  EXPECT_EQ(*stored, "brf_guest_new") << "a key already stored stays";
  EXPECT_EQ(saves_, 1);

  identity_ = LocalIdentity{};
  identity_.account_id = "account:b";
  EXPECT_FALSE(StoreBriefGuestKeyIfNone(Store(), SecretText("brf_guest_x"), "account:a"))
      << "minted for another account";
  EXPECT_TRUE(identity_.brief_llm_guest_api_key.empty());
  EXPECT_EQ(saves_, 1);
}

} // namespace
} // namespace pbr
