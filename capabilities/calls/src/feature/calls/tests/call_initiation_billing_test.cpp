#include "feature/calls/CallInitiationBilling.h"

#include "common/Utilities.h"
#include "lib/pricing/InitiationBillingStore.h"

#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <optional>
#include <string>

namespace pbr {
namespace {

/** This device's account: an id and the floor a stranger's invite must offer. */
class FakeAccount final : public ILocalAccount {
public:
  Roe<std::string> GetAccountId() const override { return std::string("me"); }
  Roe<int64_t> InitiationFloor() const override { return floor; }
  int64_t floor = 0;
};

// P001 initiation pricing as the call's invite / accept apply it (payment rails are not live yet).
class CallInitiationBillingTest : public ::testing::Test {
protected:
  void SetUp() override {
    dir_ = std::filesystem::temp_directory_path() / ("pp_browser_call_billing_" + util::GenerateUuid());
    store_ = std::make_unique<InitiationBillingStore>(dir_.string());
    billing_ = std::make_unique<CallInitiationBilling>(account_);
  }
  void TearDown() override {
    billing_.reset();
    store_.reset();
    std::error_code ec;
    std::filesystem::remove_all(dir_, ec);
  }
  void SetLocalFloor(int64_t floor) { account_.floor = floor; }

  std::filesystem::path dir_;
  FakeAccount account_;
  std::unique_ptr<InitiationBillingStore> store_;
  std::unique_ptr<CallInitiationBilling> billing_;
};

TEST_F(CallInitiationBillingTest, NoStoreMeansFreeCallsWithNoPricing) {
  SetLocalFloor(10);
  EXPECT_TRUE(billing_->CheckCanPlace({"peer"}, "me"));
  CallInviteDetail invite;
  invite.call_id = "call:1";
  EXPECT_TRUE(billing_->FillInviteOffer("peer", invite));
  EXPECT_EQ(invite.offer_amount_minor, 0);
  EXPECT_TRUE(billing_->TakeInboundOffer("peer", invite)) << "no store: our floor is not applied";
  CallAcceptDetail accept;
  billing_->FillAccept(5, InitiationChargeDecision::TakeAll, accept);
  EXPECT_EQ(accept.offer_amount_minor, 0);
  EXPECT_EQ(billing_->OfferFrom("peer"), 0);
}

TEST_F(CallInitiationBillingTest, OfferDueToAClosedPeerBlocksPlacingUntilOpen) {
  billing_->SetStore(store_.get());
  ASSERT_TRUE(store_->SetFloor("paid", 5));
  EXPECT_TRUE(billing_->CheckCanPlace({"free", "me"}, "me"));
  EXPECT_FALSE(billing_->CheckCanPlace({"free", "paid"}, "me")) << "an offer is due and cannot be paid";
  CallInviteDetail invite;
  EXPECT_FALSE(billing_->FillInviteOffer("paid", invite));

  ASSERT_TRUE(store_->MarkOpen("paid"));
  EXPECT_TRUE(billing_->CheckCanPlace({"paid"}, "me")) << "an open peer is owed nothing";
  EXPECT_TRUE(billing_->FillInviteOffer("paid", invite));
  EXPECT_EQ(invite.offer_amount_minor, 0);
}

TEST_F(CallInitiationBillingTest, TakeAllNeedsPaymentRails) {
  EXPECT_FALSE(CallInitiationBilling::CheckCanAccept(InitiationChargeDecision::TakeAll, 5));
  EXPECT_TRUE(CallInitiationBilling::CheckCanAccept(InitiationChargeDecision::TakeAll, 0));
  EXPECT_TRUE(CallInitiationBilling::CheckCanAccept(InitiationChargeDecision::Waive, 5));
}

TEST_F(CallInitiationBillingTest, InboundOfferBelowOurFloorIsRefusedElseBooked) {
  billing_->SetStore(store_.get());
  SetLocalFloor(10);
  CallInviteDetail low;
  low.call_id = "call:low";
  low.offer_amount_minor = 5;
  EXPECT_FALSE(billing_->TakeInboundOffer("caller", low));
  EXPECT_EQ(billing_->OfferFrom("caller"), 0);

  CallInviteDetail enough;
  enough.call_id = "call:ok";
  enough.offer_amount_minor = 10;
  EXPECT_TRUE(billing_->TakeInboundOffer("caller", enough));
  EXPECT_EQ(billing_->OfferFrom("caller"), 10);

  CallAcceptDetail accept;
  billing_->FillAccept(10, InitiationChargeDecision::Waive, accept);
  EXPECT_EQ(accept.offer_amount_minor, 10);
  EXPECT_EQ(accept.charge_decision, InitiationChargeDecision::Waive);
  billing_->NoteAccepted("caller");
  EXPECT_TRUE(store_->IsOpen("caller"));
}

// A stranger's price for a call is its card's (multi-relay MR005), not the directory's entry (which relays no longer
// fill): a card that charges blocks the call before any invite goes; one that is free lets it ring.
TEST_F(CallInitiationBillingTest, TheStrangersCardSaysWhetherACallCanBePlaced) {
  billing_->SetStore(store_.get());
  int64_t card_floor = 6;
  store_->SetCardFloors([&card_floor](const std::string& peer) -> std::optional<InitiationCardFloor> {
    if (peer != "account:stranger") {
      return std::nullopt;
    }
    return InitiationCardFloor{.floor_minor = card_floor, .issued_at = 10};
  });
  ASSERT_TRUE(store_->SetFloor("account:stranger", 0)); // the directory hit says free
  EXPECT_FALSE(billing_->CheckCanPlace({"account:stranger"}, "me")) << "its card charges";
  CallInviteDetail invite;
  EXPECT_FALSE(billing_->FillInviteOffer("account:stranger", invite));

  card_floor = 0;
  EXPECT_TRUE(billing_->CheckCanPlace({"account:stranger"}, "me")) << "its card is free";
  EXPECT_TRUE(billing_->FillInviteOffer("account:stranger", invite));
  EXPECT_EQ(invite.offer_amount_minor, 0);
  EXPECT_EQ(invite.floor_minor, 0);
}

} // namespace
} // namespace pbr
