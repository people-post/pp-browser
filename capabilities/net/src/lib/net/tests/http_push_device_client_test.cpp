// This device's push binding on one relay (multi-relay MR007 item 5): an unregister names the binding the last register
// made, so a late one leaves a newer binding alone.
#include "common/ValueJson.h"
#include "foundation/sockets/testing/ScriptedHttpServer.h"
#include "foundation/error/AppError.h"
#include "lib/net/OrgBackendClientsImpl.h"
#include "lib/net/RelayAudience.h"

#include "lib/net/testing/FakeRelaySigner.h"

#include <gtest/gtest.h>

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/PbrCompat.h"

namespace pbr {
namespace {

using test::ScriptedHttpServer;
using Reply = ScriptedHttpServer::Reply;

constexpr const char kAudience[] = "QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP";

/** The relay's device rows: a new binding_id per register; an unregister of another id is binding_mismatch. */
class PushDeviceClientTest : public ::testing::Test {
protected:
  void SetUp() override {
    client_.SetAuthSigner(test::FakeRelaySigner());
    client_.SetAudience(std::make_shared<FixedRelayAudience>(kAudience));
    server_.SetHandler([this](const ScriptedHttpServer::Request& request) {
      std::lock_guard lock(mu_);
      const Object body = TryParseObject(request.body).value_or(Object{});
      bodies_.push_back(body);
      if (request.target.ends_with("/v1/devices/register")) {
        held_ = "bind-" + std::to_string(++registers_);
        return Reply{.status = 200, .headers = {}, .body = R"({"success":true,"binding_id":")" + held_ + R"("})"};
      }
      if (body.getString("binding_id").value_or("") != held_) {
        return Reply{.status = 409, .headers = {}, .body = R"({"error":"x","code":"binding_mismatch"})"};
      }
      held_.clear();
      return Reply{.status = 200, .headers = {}, .body = R"({"success":true})"};
    });
  }

  PushDeviceRegistration Device() const {
    return PushDeviceRegistration{
        .platform = "android", .push_token = "tok", .device_id = "dev-1", .account_id = "account:me"};
  }

  ScriptedHttpServer server_;
  HttpPushDeviceClient client_{server_.Origin() + "/api/relay"};
  std::mutex mu_;
  std::vector<Object> bodies_;
  std::string held_;
  int registers_ = 0;
};

// MR007-SCOPE: the unregister names the binding the register answered, signed for the relay.
TEST_F(PushDeviceClientTest, AnUnregisterNamesTheBindingTheRegisterMade) {
  ASSERT_TRUE(client_.RegisterDevice(Device()));
  ASSERT_TRUE(client_.UnregisterDevice(Device()));
  std::lock_guard lock(mu_);
  ASSERT_EQ(bodies_.size(), 2u);
  EXPECT_EQ(bodies_[0].getString("push_token").value_or(""), "tok");
  EXPECT_EQ(bodies_[0].getString("audience").value_or(""), kAudience);
  EXPECT_EQ(bodies_[0].getIf<int64_t>("sign_version"), 4) << "relay-api's version byte (SIGNED_FORMATS § 2.8)";
  EXPECT_EQ(bodies_[1].getString("binding_id").value_or(""), "bind-1");
  EXPECT_FALSE(bodies_[1].contains("push_token")) << "the token is no part of a removal";
  EXPECT_TRUE(held_.empty());
}

// MR007-SCOPE: none known (a binding another start made), or a newer one there: register, then unregister its answer.
TEST_F(PushDeviceClientTest, WithoutAKnownBindingItRegistersFirstThenUnregistersOnce) {
  ASSERT_TRUE(client_.UnregisterDevice(Device()));
  std::lock_guard lock(mu_);
  ASSERT_EQ(bodies_.size(), 2u);
  EXPECT_TRUE(bodies_[0].contains("push_token")) << "a register first";
  EXPECT_EQ(bodies_[1].getString("binding_id").value_or(""), "bind-1");
}

TEST_F(PushDeviceClientTest, ABindingMismatchRegistersAndUnregistersTheCurrentOneOnce) {
  ASSERT_TRUE(client_.RegisterDevice(Device()));
  {
    std::lock_guard lock(mu_);
    held_ = "bind-other-start"; // the relay holds a newer binding than ours
  }
  ASSERT_TRUE(client_.UnregisterDevice(Device()));
  std::lock_guard lock(mu_);
  ASSERT_EQ(bodies_.size(), 4u);
  EXPECT_EQ(bodies_[1].getString("binding_id").value_or(""), "bind-1");
  EXPECT_TRUE(bodies_[2].contains("push_token"));
  EXPECT_EQ(bodies_[3].getString("binding_id").value_or(""), "bind-2");
  EXPECT_TRUE(held_.empty());
}

/** A relay that holds one binding per device id, as the real one does. */
class PushDeviceSiblingTest : public ::testing::Test {
protected:
  void SetUp() override {
    client_.SetAuthSigner(test::FakeRelaySigner());
    client_.SetAudience(std::make_shared<FixedRelayAudience>(kAudience));
    server_.SetHandler([this](const ScriptedHttpServer::Request& request) {
      std::lock_guard lock(mu_);
      const Object body = TryParseObject(request.body).value_or(Object{});
      bodies_.push_back(body);
      const std::string device = body.getString("device_id").value_or("");
      if (request.target.ends_with("/v1/devices/register")) {
        held_[device] = "bind-" + std::to_string(++registers_);
        return Reply{
            .status = 200, .headers = {}, .body = R"({"success":true,"binding_id":")" + held_[device] + R"("})"};
      }
      if (body.getString("binding_id").value_or("") != held_[device]) {
        return Reply{.status = 409, .headers = {}, .body = R"({"error":"x","code":"binding_mismatch"})"};
      }
      held_.erase(device);
      return Reply{.status = 200, .headers = {}, .body = R"({"success":true})"};
    });
  }

  ScriptedHttpServer server_;
  HttpPushDeviceClient client_{server_.Origin() + "/api/relay"};
  std::mutex mu_;
  std::vector<Object> bodies_;
  std::map<std::string, std::string> held_;
  int registers_ = 0;
};

// The `.voip` sibling's register must not displace the main device's binding id: each unregister names its own.
TEST_F(PushDeviceSiblingTest, EachDeviceKeepsItsOwnBindingIdForItsUnregister) {
  const PushDeviceRegistration main{
      .platform = "ios", .push_token = "tok", .device_id = "dev-1", .account_id = "account:me"};
  const PushDeviceRegistration voip{
      .platform = "ios_voip", .push_token = "voip-tok", .device_id = "dev-1.voip", .account_id = "account:me"};
  ASSERT_TRUE(client_.RegisterDevice(main));
  ASSERT_TRUE(client_.RegisterDevice(voip));
  ASSERT_TRUE(client_.UnregisterDevice(main));
  ASSERT_TRUE(client_.UnregisterDevice(voip));
  std::lock_guard lock(mu_);
  ASSERT_EQ(bodies_.size(), 4u) << "no register-first fallback";
  EXPECT_EQ(bodies_[2].getString("device_id").value_or(""), "dev-1");
  EXPECT_EQ(bodies_[2].getString("binding_id").value_or(""), "bind-1");
  EXPECT_EQ(bodies_[3].getString("device_id").value_or(""), "dev-1.voip");
  EXPECT_EQ(bodies_[3].getString("binding_id").value_or(""), "bind-2");
  EXPECT_TRUE(held_.empty());
}

} // namespace
} // namespace pbr
