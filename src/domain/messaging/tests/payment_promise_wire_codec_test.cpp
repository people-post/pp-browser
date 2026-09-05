#include "domain/messaging/PaymentPromiseWireCodec.h"
#include "domain/messaging/PaymentPromiseCodec.h"
#include "foundation/data/PaymentPromiseTypes.h"

#include <gtest/gtest.h>
#include "common/PbrCompat.h"

namespace {

using namespace pbr;

PaymentPromise SamplePromise() {
  PaymentPromise promise;
  promise.promise_id = "promise:wire-1";
  promise.payer_account_id = "account:alice";
  promise.payee_account_id = "account:bob";
  promise.amount_minor = 10;
  promise.currency = kPricingCurrencyId;
  promise.service_ref = "service:wire";
  promise.state = PaymentPromiseState::Offered;
  promise.created_at_ms = 1700000000000;
  promise.payer_signature_b64 = "c2ln";
  return promise;
}

TEST(PaymentPromiseWireCodecTest, ControlTypeRoundTrip) {
  EXPECT_EQ(PaymentPromiseWireCodec::ControlTypeToWire(PaymentPromiseControlType::PromiseOffer), "promise_offer");
  EXPECT_EQ(PaymentPromiseWireCodec::ControlTypeFromWire("promise_accept"),
            PaymentPromiseControlType::PromiseAccept);
  EXPECT_EQ(PaymentPromiseWireCodec::ControlTypeFromWire("promise_outcome"),
            PaymentPromiseControlType::PromiseOutcome);
  EXPECT_FALSE(PaymentPromiseWireCodec::ControlTypeFromWire("nope").has_value());
}

TEST(PaymentPromiseWireCodecTest, BuildAndParseSystemMessage) {
  auto message = PaymentPromiseWireCodec::BuildSystemMessage("thread-1", PaymentPromiseControlType::PromiseOffer,
                                                             SamplePromise(), "Payment promise offered");
  ASSERT_TRUE(static_cast<bool>(message)) << message.error().message;
  EXPECT_EQ(message->thread_id, "thread-1");
  EXPECT_EQ(message->content_type, ChatContentType::System);
  EXPECT_EQ(message->text, "Payment promise offered");
  ASSERT_TRUE(PaymentPromiseWireCodec::ControlTypeFromMessage(*message).has_value());
  EXPECT_EQ(*PaymentPromiseWireCodec::ControlTypeFromMessage(*message), PaymentPromiseControlType::PromiseOffer);

  auto root_type = PaymentPromiseWireCodec::ControlTypeFromMessage(*message);
  ASSERT_TRUE(root_type.has_value());
  // Decode detail via Encode/Decode path used by ingest.
  auto encoded = PaymentPromiseWireCodec::EncodeDetail(SamplePromise());
  ASSERT_TRUE(static_cast<bool>(encoded));
  auto decoded = PaymentPromiseWireCodec::DecodeDetail(*encoded);
  ASSERT_TRUE(static_cast<bool>(decoded)) << decoded.error().message;
  EXPECT_EQ(decoded->promise_id, "promise:wire-1");
  EXPECT_EQ(decoded->amount_minor, 10);
  EXPECT_EQ(decoded->payer_signature_b64, "c2ln");
}

} // namespace
