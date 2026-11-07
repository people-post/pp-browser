// B183: what renewing the Brief LLM key after an HTTP 401 does to the stored identity.
#include "domain/registration/BriefLlmKeyRenewal.h"

#include "foundation/error/AppError.h"

#include <gtest/gtest.h>

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
    store.save = [this](const LocalIdentity& identity) -> Roe<void> {
      ++saves_;
      if (!save_ok_) {
        return AppError::Internal("disk full");
      }
      identity_ = identity;
      return {};
    };
    store.mint = [this](const std::string& base_url) -> Roe<BriefGuestLlmMintResult> {
      mint_urls_.push_back(base_url);
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

} // namespace
} // namespace pbr
