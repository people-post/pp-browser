#include "app/node/NodeHeartbeat.h"

#include "common/ValueJson.h"

#include "common/metrics/MetricsRegistry.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace {

struct Sent {
  std::string url;
  pbr::Object body;
  long timeout_s = 0;
};

// Scripted transport + manual clocks; `network_error` makes the post fail.
struct Fixture {
  std::vector<Sent> sent;
  std::vector<std::pair<long, std::string>> replies;  // consumed in order; the last repeats
  bool network_error = false;
  int reregisters = 0;
  bool reregister_ok = true;
  bool registered = true;
  int64_t unix_ms = 1'700'000'000'000;
  int64_t steady_ms = 1'000;

  pbr::NodeHeartbeatDeps Deps() {
    pbr::NodeHeartbeatDeps d;
    d.base_url = "https://relay.example/api";
    d.identity = [this] {
      return registered ? pbr::HeartbeatIdentity{"relay:u1", "peer1"} : pbr::HeartbeatIdentity{};
    };
    d.sign = [](const std::string&, const std::string&, const std::string& state, int64_t ts) -> pbr::Roe<std::string> {
      return "sig-" + state + "-" + std::to_string(ts);
    };
    d.post = [this](const std::string& url, const std::string& body, const std::map<std::string, std::string>&,
                    long timeout_s) -> pbr::Roe<pbr::HttpResponse> {
      Sent s;
      s.url = url;
      s.body = *pbr::TryParseObject(body);
      s.timeout_s = timeout_s;
      sent.push_back(std::move(s));
      if (network_error) {
        return pbr::Error("connection refused");
      }
      const size_t i = std::min(sent.size() - 1, replies.size() - 1);
      return pbr::HttpResponse{replies[i].first, replies[i].second};
    };
    d.reregister = [this]() -> pbr::Roe<bool> {
      ++reregisters;
      if (!reregister_ok) {
        return pbr::Error("register failed");
      }
      return true;
    };
    d.unix_ms = [this] { return unix_ms; };
    d.steady_ms = [this] { return steady_ms; };
    return d;
  }

  void Advance(int64_t ms) {
    steady_ms += ms;
    unix_ms += ms;
  }
};

constexpr const char* kOk = R"({"success":true,"server_time":1700000000000,"state":"active"})";
constexpr int64_t kPeriod = pbr::kHeartbeatPeriodMs;

int64_t Ts(const Sent& s) {
  return static_cast<int64_t>(*s.body.getNonNegInt("timestamp"));
}

} // namespace

TEST(NodeHeartbeatTest, SendsSignedActiveHeartbeatToContractEndpoint) {
  Fixture f;
  f.replies = {{200, kOk}};
  pbr::NodeHeartbeat hb(f.Deps());
  hb.Tick();
  ASSERT_EQ(f.sent.size(), 1u);
  EXPECT_EQ(f.sent[0].url, "https://relay.example/api/v1/mesh/heartbeat");
  EXPECT_EQ(f.sent[0].timeout_s, pbr::kHeartbeatTimeoutS);
  EXPECT_EQ(f.sent[0].body.getString("relay_user_id").value_or(""), "relay:u1");
  EXPECT_EQ(f.sent[0].body.getString("peer_id").value_or(""), "peer1");
  EXPECT_EQ(f.sent[0].body.getString("state").value_or(""), "active");
  EXPECT_EQ(f.sent[0].body.getString("signature").value_or(""), "sig-active-" + std::to_string(Ts(f.sent[0])));
}

TEST(NodeHeartbeatTest, WaitsForThePeriodBetweenSends) {
  Fixture f;
  f.replies = {{200, kOk}};
  pbr::NodeHeartbeat hb(f.Deps());
  hb.Tick();
  f.Advance(kPeriod - 1);
  hb.Tick();
  EXPECT_EQ(f.sent.size(), 1u);
  f.Advance(1);
  hb.Tick();
  EXPECT_EQ(f.sent.size(), 2u);
}

TEST(NodeHeartbeatTest, UnregisteredNodeSendsNothingThenStartsOnceRegistered) {
  Fixture f;
  f.replies = {{200, kOk}};
  f.registered = false;
  pbr::NodeHeartbeat hb(f.Deps());
  hb.Tick();
  hb.SendFinal();
  EXPECT_TRUE(f.sent.empty());
  f.registered = true;
  hb.Tick();
  EXPECT_EQ(f.sent.size(), 1u);
}

TEST(NodeHeartbeatTest, StateChangeSendsOnNextTickWithoutWaiting) {
  Fixture f;
  f.replies = {{200, kOk}};
  pbr::NodeHeartbeat hb(f.Deps());
  hb.Tick();
  f.Advance(1000);
  EXPECT_TRUE(hb.SetState(pbr::HeartbeatState::Draining));
  hb.Tick();
  ASSERT_EQ(f.sent.size(), 2u);
  EXPECT_EQ(f.sent[1].body.getString("state").value_or(""), "draining");
  f.Advance(1000);
  hb.Tick();
  EXPECT_EQ(f.sent.size(), 2u) << "the change is sent once, then the normal period applies";
}

TEST(NodeHeartbeatTest, StateMovesForwardOnlyAndRetiringIsFinal) {
  Fixture f;
  pbr::NodeHeartbeat hb(f.Deps());
  EXPECT_TRUE(hb.SetState(pbr::HeartbeatState::Draining));
  EXPECT_TRUE(hb.SetState(pbr::HeartbeatState::Active)) << "a drill can be undone";
  EXPECT_TRUE(hb.SetState(pbr::HeartbeatState::Retiring));
  EXPECT_FALSE(hb.SetState(pbr::HeartbeatState::Active));
  EXPECT_FALSE(hb.SetState(pbr::HeartbeatState::Draining));
  EXPECT_TRUE(hb.SetState(pbr::HeartbeatState::Retiring));
  EXPECT_EQ(hb.State(), pbr::HeartbeatState::Retiring);
}

TEST(NodeHeartbeatTest, ParsesOnlyKnownStates) {
  EXPECT_EQ(pbr::ParseHeartbeatState("draining"), pbr::HeartbeatState::Draining);
  EXPECT_FALSE(pbr::ParseHeartbeatState("sleeping"));
  EXPECT_FALSE(pbr::ParseHeartbeatState(""));
}

TEST(NodeHeartbeatTest, TimestampsStrictlyIncreaseEvenWhenClockStalls) {
  Fixture f;
  f.replies = {{200, kOk}};
  pbr::NodeHeartbeat hb(f.Deps());
  hb.Tick();
  hb.SetState(pbr::HeartbeatState::Draining);
  hb.Tick();  // same unix_ms
  hb.SetState(pbr::HeartbeatState::Active);
  f.unix_ms -= 5000;  // clock stepped back
  hb.Tick();
  ASSERT_EQ(f.sent.size(), 3u);
  EXPECT_LT(Ts(f.sent[0]), Ts(f.sent[1]));
  EXPECT_LT(Ts(f.sent[1]), Ts(f.sent[2]));
}

TEST(NodeHeartbeatTest, NotRegisteredReRegistersWithBackoffAndSuccessResets) {
  Fixture f;
  f.replies = {{404, R"({"error":"x","code":"not_registered"})"}};
  pbr::NodeHeartbeat hb(f.Deps());
  hb.Tick();  // 404 -> re-register now, retry right away
  EXPECT_EQ(f.reregisters, 1);
  hb.Tick();  // 404 again, backoff (10s) not elapsed
  EXPECT_EQ(f.reregisters, 1);
  ASSERT_EQ(f.sent.size(), 2u);
  f.Advance(5'000);
  hb.Tick();
  EXPECT_EQ(f.sent.size(), 2u) << "waits for the backoff";
  f.Advance(5'000);
  hb.Tick();  // backoff elapsed: send 404s, re-register (delay now 20s)
  EXPECT_EQ(f.reregisters, 2);
  f.Advance(20'000);
  hb.Tick();
  EXPECT_EQ(f.reregisters, 3);
  EXPECT_LE(f.sent.size(), 6u);

  f.replies = {{200, kOk}};
  const size_t before = f.sent.size();
  f.Advance(kPeriod);
  hb.Tick();
  ASSERT_EQ(f.sent.size(), before + 1);
  f.replies = {{404, R"({"code":"registration_expired"})"}};
  f.Advance(kPeriod);
  const int regs = f.reregisters;
  hb.Tick();
  EXPECT_EQ(f.reregisters, regs + 1) << "a success reset the backoff";
}

TEST(NodeHeartbeatTest, FailedReRegistrationWaitsForBackoff) {
  Fixture f;
  f.replies = {{404, R"({"code":"not_registered"})"}};
  f.reregister_ok = false;
  pbr::NodeHeartbeat hb(f.Deps());
  hb.Tick();
  EXPECT_EQ(f.reregisters, 1);
  const size_t sent = f.sent.size();
  f.Advance(9'000);
  hb.Tick();
  EXPECT_EQ(f.sent.size(), sent);
  EXPECT_EQ(f.reregisters, 1);
}

TEST(NodeHeartbeatTest, UnknownPeerDoesNotReRegisterAndRetriesNextPeriod) {
  Fixture f;
  f.replies = {{404, R"({"code":"unknown_peer"})"}};
  pbr::NodeHeartbeat hb(f.Deps());
  hb.Tick();
  f.Advance(1000);
  hb.Tick();
  EXPECT_EQ(f.reregisters, 0);
  EXPECT_EQ(f.sent.size(), 1u);
  f.Advance(kPeriod);
  hb.Tick();
  EXPECT_EQ(f.sent.size(), 2u);
}

TEST(NodeHeartbeatTest, StaleTimestampSkipsToNextPeriod) {
  Fixture f;
  f.replies = {{409, R"({"code":"stale_timestamp"})"}};
  pbr::NodeHeartbeat hb(f.Deps());
  hb.Tick();
  f.Advance(1000);
  hb.Tick();
  EXPECT_EQ(f.sent.size(), 1u);
  EXPECT_EQ(f.reregisters, 0);
}

TEST(NodeHeartbeatTest, UnauthorizedIsCountedAndRetriedNextPeriod) {
  Fixture f;
  f.replies = {{401, R"({"error":"bad signature","server_time":1700000200000})"}};  // 200 s ahead
  pbr::NodeHeartbeat hb(f.Deps());
  hb.Tick();
  f.Advance(1000);
  hb.Tick();
  EXPECT_EQ(f.sent.size(), 1u);
  EXPECT_EQ(f.reregisters, 0);
  const std::string text = pbr::MetricsRegistry::Global().RenderPrometheus();
  EXPECT_NE(text.find("pp_node_heartbeat_total{result=\"auth\"} 1"), std::string::npos) << text;
  EXPECT_NE(text.find("pp_node_heartbeat_clock_skew_seconds 200"), std::string::npos) << text;
}

TEST(NodeHeartbeatTest, NetworkAndServerErrorsRetryNextPeriodWithoutStacking) {
  Fixture f;
  f.network_error = true;
  pbr::NodeHeartbeat hb(f.Deps());
  hb.Tick();
  f.Advance(1000);
  hb.Tick();
  EXPECT_EQ(f.sent.size(), 1u);
  f.network_error = false;
  f.replies = {{503, "unavailable"}, {429, ""}};
  f.Advance(kPeriod);
  hb.Tick();
  EXPECT_EQ(f.sent.size(), 2u);
  f.Advance(kPeriod);
  hb.Tick();
  EXPECT_EQ(f.sent.size(), 3u);
  EXPECT_EQ(f.reregisters, 0);
}

TEST(NodeHeartbeatTest, SendFinalReportsRetiringWithShortTimeoutAndSurvivesFailure) {
  Fixture f;
  f.network_error = true;
  pbr::NodeHeartbeat hb(f.Deps());
  hb.SendFinal();
  ASSERT_EQ(f.sent.size(), 1u);
  EXPECT_EQ(f.sent[0].body.getString("state").value_or(""), "retiring");
  EXPECT_EQ(f.sent[0].timeout_s, pbr::kHeartbeatFinalTimeoutS);
  EXPECT_EQ(f.reregisters, 0);
  EXPECT_EQ(hb.State(), pbr::HeartbeatState::Retiring);
}
