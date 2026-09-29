#include "gui/CallMetricsTracker.h"

#include <gtest/gtest.h>

#include <regex>
#include <string>
#include <vector>

namespace pbr {
namespace {

using Tracker = CallMetricsTracker;

constexpr const char* kCall = "call:0123456789abcdef0123456789abcdef";

DeviceVitals Vitals(double cpu_s = 10.0, int thermal = 0, int battery = 80) {
  DeviceVitals v;
  v.cpu_s = cpu_s;
  v.thermal = thermal;
  v.battery_pct = battery;
  return v;
}

Tracker::MediaTick Media(bool connected, uint64_t rx_audio = 0, uint64_t rx_video = 0,
                         CallPathQuality q = CallPathQuality::Excellent, const char* path = "direct") {
  Tracker::MediaTick t;
  t.connected = connected;
  t.path = path;
  t.quality = q;
  t.health.rx_audio_frames = rx_audio;
  t.health.tx_audio_frames = rx_audio;
  t.health.rx_video_frames = rx_video;
  t.health.tx_video_frames = rx_video;
  t.health.audio_io = "vpio";
  return t;
}

bool Has(const std::string& line, const std::string& kv) {
  return (" " + line + " ").find(" " + kv + " ") != std::string::npos;
}

/** Drive Tick with call_live=false until the call ends; returns the final line. */
std::string EndCall(Tracker& t, int64_t& now, const DeviceVitals& v = Vitals()) {
  std::string last;
  for (int i = 0; i < 10 && t.Tracking(); ++i) {
    now += 500;
    for (const std::string& line : t.Tick(false, now, v)) {
      last = line;
    }
  }
  return last;
}

TEST(CallMetricsLineTest, FormatsKeyValuesAndShortCallId) {
  const std::string line = MetricsLine("x.y").Add("a", "b").Add("n", int64_t{3}).Add("f", 2.25).Add("e", "").str();
  EXPECT_EQ(line, "event=x.y a=b n=3 f=2.2 e=-");  // one decimal; empty value never breaks the key=value form
  EXPECT_EQ(MetricsCallId(kCall), "01234567");
  EXPECT_EQ(MetricsCallId("abc"), "abc");
}

TEST(CallMetricsTrackerTest, CalleeConnectedCallReportsSetupAndSummary) {
  Tracker t;
  int64_t now = 100'000;
  EXPECT_TRUE(t.NoteRing(kCall, true, now, Vitals(10.0)).empty());
  t.NoteAccept(kCall, false, now + 2000);
  EXPECT_TRUE(t.NoteMedia(kCall, Media(false), now + 2500, Vitals()).empty()) << "not connected yet";
  const auto setup = t.NoteMedia(kCall, Media(true), now + 3000, Vitals());
  ASSERT_EQ(setup.size(), 1u);
  EXPECT_TRUE(Has(setup[0], "event=call.setup")) << setup[0];
  EXPECT_TRUE(Has(setup[0], "role=callee"));
  EXPECT_TRUE(Has(setup[0], "video=1"));
  EXPECT_TRUE(Has(setup[0], "result=connected"));
  EXPECT_TRUE(Has(setup[0], "answer_ms=2000"));
  EXPECT_TRUE(Has(setup[0], "connect_ms=1000"));
  EXPECT_TRUE(Has(setup[0], "path=direct"));

  now += 3000;
  for (int i = 1; i <= 10; ++i) {  // 10 s connected; audio from 1 s, video from 2 s
    now += 1000;
    t.NoteMedia(kCall, Media(true, i >= 1 ? 50u * i : 0u, i >= 2 ? 20u * (i - 1) : 0u), now, Vitals());
    t.Tick(true, now, Vitals());
  }
  const std::string summary = EndCall(t, now, Vitals(14.5, 0, 78));
  EXPECT_TRUE(Has(summary, "event=call.summary")) << summary;
  EXPECT_TRUE(Has(summary, "duration_s=10.0")) << summary;
  EXPECT_TRUE(Has(summary, "first_audio_ms=1000")) << summary;
  EXPECT_TRUE(Has(summary, "first_video_ms=2000")) << summary;
  EXPECT_TRUE(Has(summary, "rx_audio=500")) << summary;
  EXPECT_TRUE(Has(summary, "rx_fps=18.0")) << summary;  // 180 frames / 10 s
  EXPECT_TRUE(Has(summary, "q_excellent_pct=100")) << summary;
  EXPECT_TRUE(Has(summary, "cpu_s=4.5")) << summary;
  EXPECT_TRUE(Has(summary, "battery_start=80")) << summary;
  EXPECT_TRUE(Has(summary, "battery_end=78")) << summary;
  EXPECT_FALSE(t.Tracking());
}

TEST(CallMetricsTrackerTest, UnansweredRingIsMissedAndDeclineIsDeclined) {
  Tracker t;
  int64_t now = 1000;
  t.NoteRing(kCall, false, now, Vitals());
  now += 20'000;
  t.Tick(true, now, Vitals());
  std::string line = EndCall(t, now);
  EXPECT_TRUE(Has(line, "event=call.setup")) << line;
  EXPECT_TRUE(Has(line, "result=missed")) << line;

  t.NoteRing("call:ffffffff00", false, now, Vitals());
  t.NoteDecline("call:ffffffff00");
  line = EndCall(t, now);
  EXPECT_TRUE(Has(line, "result=declined")) << line;
}

TEST(CallMetricsTrackerTest, CallerThatNeverConnectsReportsHowLongItWaited) {
  Tracker t;
  int64_t now = 1000;
  t.NoteOutbound(kCall, true, now, Vitals());
  now += 30'000;
  t.Tick(true, now, Vitals());
  const std::string line = EndCall(t, now);
  EXPECT_TRUE(Has(line, "result=not_connected")) << line;
  EXPECT_TRUE(Has(line, "role=caller")) << line;
  EXPECT_TRUE(Has(line, "waited_ms=30000")) << line;
}

// The callee's accept can wait 12 s for the relay (B50) with no active call on screen yet: as long
// as the lifecycle says a call is live, the call is not over.
TEST(CallMetricsTrackerTest, LiveCallSurvivesTheAcceptGap) {
  Tracker t;
  int64_t now = 1000;
  t.NoteRing(kCall, true, now, Vitals());
  t.NoteAccept(kCall, false, now);
  for (int i = 0; i < 24; ++i) {
    now += 500;
    EXPECT_TRUE(t.Tick(true, now, Vitals()).empty());
  }
  EXPECT_TRUE(t.Tracking());
  const auto setup = t.NoteMedia(kCall, Media(true), now, Vitals());
  ASSERT_EQ(setup.size(), 1u);
  EXPECT_TRUE(Has(setup[0], "connect_ms=12000")) << setup[0];
}

TEST(CallMetricsTrackerTest, CallerTimesConnectFromPlacingTheCall) {
  Tracker t;
  int64_t now = 1000;
  t.NoteOutbound(kCall, true, now, Vitals());
  const auto setup = t.NoteMedia(kCall, Media(true), now + 7000, Vitals());
  ASSERT_EQ(setup.size(), 1u);
  EXPECT_TRUE(Has(setup[0], "connect_ms=7000")) << setup[0];
  EXPECT_FALSE(Has(setup[0], "answer_ms=0"));
}

TEST(CallMetricsTrackerTest, UiThreadStuckDuringACallIsAStallButSuspensionIsNot) {
  Tracker t;
  EXPECT_TRUE(t.NoteUiLatency(5000).empty()) << "no call: nothing to attribute it to";
  t.NoteOutbound(kCall, false, 1000, Vitals());
  EXPECT_TRUE(t.NoteUiLatency(40).empty()) << "a normal frame";
  auto lines = t.NoteUiLatency(2500);
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(Has(lines[0], "event=ui.stall")) << lines[0];
  EXPECT_TRUE(Has(lines[0], "stuck_ms=2500")) << lines[0];
  EXPECT_TRUE(t.NoteUiLatency(120'000).empty()) << "the app was suspended";
}

TEST(CallMetricsTrackerTest, ANewCallFinishesThePreviousOne) {
  Tracker t;
  int64_t now = 1000;
  t.NoteRing(kCall, false, now, Vitals());
  const auto lines = t.NoteOutbound("call:99999999aa", false, now + 1000, Vitals());
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_TRUE(Has(lines[0], "call=01234567")) << lines[0];
  EXPECT_TRUE(Has(lines[0], "result=missed")) << lines[0];
  EXPECT_TRUE(t.Tracking());
}

TEST(CallMetricsTrackerTest, CountsReconnectsPathChangesAndThermalChanges) {
  Tracker t;
  int64_t now = 1000;
  t.NoteOutbound(kCall, true, now, Vitals(1.0, 0));
  t.NoteMedia(kCall, Media(true), now += 1000, Vitals(1.0, 0));
  t.NoteMedia(kCall, Media(false), now += 1000, Vitals(1.0, 0));
  t.NoteMedia(kCall, Media(false), now += 1000, Vitals(1.0, 0));
  t.NoteMedia(kCall, Media(true, 0, 0, CallPathQuality::Poor, "relayed"), now += 1000, Vitals(1.0, 0));
  const auto hot = t.NoteMedia(kCall, Media(true, 0, 0, CallPathQuality::Poor, "relayed"), now += 1000, Vitals(1.0, 2));
  ASSERT_EQ(hot.size(), 1u);
  EXPECT_TRUE(Has(hot[0], "event=device.thermal")) << hot[0];
  EXPECT_TRUE(Has(hot[0], "state=serious")) << hot[0];
  const std::string summary = EndCall(t, now, Vitals(2.0, 1));
  EXPECT_TRUE(Has(summary, "reconnects=1")) << summary;
  EXPECT_TRUE(Has(summary, "path_changes=1")) << summary;
  EXPECT_TRUE(Has(summary, "path=relayed")) << summary;
  EXPECT_TRUE(Has(summary, "thermal_max=serious")) << summary;
}

// Monitoring must never learn who talked to whom or from where.
TEST(CallMetricsTrackerTest, LinesCarryNoIdentityOrAddress) {
  Tracker t;
  int64_t now = 1000;
  std::vector<std::string> all;
  auto keep = [&](std::vector<std::string> lines) { all.insert(all.end(), lines.begin(), lines.end()); };
  keep(t.NoteRing(kCall, true, now, Vitals()));
  t.NoteAccept(kCall, false, now + 100);
  keep(t.NoteMedia(kCall, Media(true, 10, 10), now += 1000, Vitals()));
  keep(t.NoteMedia(kCall, Media(true, 60, 30), now += 2000, Vitals(10.0, 1)));
  all.push_back(EndCall(t, now));
  ASSERT_GE(all.size(), 3u);
  const std::regex ip(R"((\d{1,3}\.){3}\d{1,3}|[0-9a-f]{1,4}:[0-9a-f]{1,4}:[0-9a-f]{0,4}:)");
  for (const std::string& line : all) {
    EXPECT_EQ(line.find("account:"), std::string::npos) << line;
    EXPECT_EQ(line.find("Qm"), std::string::npos) << line;
    EXPECT_EQ(line.find("/ip"), std::string::npos) << line;
    EXPECT_FALSE(std::regex_search(line, ip)) << line;
    EXPECT_EQ(line.find("0123456789abcdef0123456789abcdef"), std::string::npos) << "full call id: " << line;
  }
}

} // namespace
} // namespace pbr
