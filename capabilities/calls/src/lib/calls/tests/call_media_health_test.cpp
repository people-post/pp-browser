#include "common/media/CallMediaHealth.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

CallMediaHealthInput BaseHealthyInput(int64_t now_ms = 10'000) {
  CallMediaHealthInput in;
  in.now_ms = now_ms;
  in.engine.active = true;
  in.engine.rx_audio_frames = 100;
  in.engine.tx_audio_frames = 100;
  in.engine.last_rx_audio_ms = now_ms - 100;
  in.engine.last_tx_audio_ms = now_ms - 100;
  in.engine.opus_target_bps = 24'000;
  return in;
}

TEST(CallMediaHealthTest, ExcellentWhenLowPressure) {
  const auto v = EvaluateCallMediaHealth(BaseHealthyInput());
  EXPECT_EQ(v.quality, CallPathQuality::Excellent);
  EXPECT_EQ(v.quality_bars, 4);
  EXPECT_EQ(v.asymmetry, CallAudioAsymmetry::None);
  EXPECT_STREQ(CallPathQualityLabelKey(v.quality), "");
}

TEST(CallMediaHealthTest, FairWhenModeratePressure) {
  auto in = BaseHealthyInput();
  in.engine.path_pressure = 0.5;
  const auto v = EvaluateCallMediaHealth(in);
  EXPECT_EQ(v.quality, CallPathQuality::Fair);
  EXPECT_EQ(v.quality_bars, 2);
  EXPECT_STREQ(CallPathQualityLabelKey(v.quality), "call.quality.fair");
}

TEST(CallMediaHealthTest, LifetimeUnderrunsDoNotForceFair) {
  auto in = BaseHealthyInput();
  in.engine.playout_underruns = 5000;
  in.engine.path_pressure = 0.0;
  EXPECT_EQ(EvaluateCallMediaHealth(in).quality, CallPathQuality::Excellent);
}

TEST(CallMediaHealthTest, LifetimeHopDropsDoNotForcePoor) {
  auto in = BaseHealthyInput();
  in.hop.drops_total = 9000;
  in.hop.path_pressure = 0.0;
  EXPECT_EQ(EvaluateCallMediaHealth(in).quality, CallPathQuality::Excellent);
}

TEST(CallMediaHealthTest, HighHopPressureIsPoor) {
  auto in = BaseHealthyInput();
  in.hop.path_pressure = 0.8;
  EXPECT_EQ(EvaluateCallMediaHealth(in).quality, CallPathQuality::Poor);
  EXPECT_EQ(EvaluateCallMediaHealth(in).quality_bars, 1);
}

TEST(CallMediaHealthTest, NoAudioWhenSendingOnly) {
  auto in = BaseHealthyInput();
  in.engine.rx_audio_frames = 0;
  in.engine.last_rx_audio_ms = 0;
  const auto v = EvaluateCallMediaHealth(in);
  EXPECT_EQ(v.quality, CallPathQuality::NoAudio);
  EXPECT_EQ(v.asymmetry, CallAudioAsymmetry::SendingOnly);
  EXPECT_EQ(v.quality_bars, 0);
}

TEST(CallMediaHealthTest, ReceivingOnlyWhenMutedFalseAndNoTx) {
  auto in = BaseHealthyInput();
  in.engine.tx_audio_frames = 0;
  in.engine.last_tx_audio_ms = 0;
  in.engine.muted = false;
  const auto v = EvaluateCallMediaHealth(in);
  EXPECT_EQ(v.asymmetry, CallAudioAsymmetry::ReceivingOnly);
  EXPECT_EQ(v.quality, CallPathQuality::Poor);
}

TEST(CallMediaHealthTest, MutedDoesNotFlagReceivingOnly) {
  auto in = BaseHealthyInput();
  in.engine.tx_audio_frames = 0;
  in.engine.last_tx_audio_ms = 0;
  in.engine.muted = true;
  const auto v = EvaluateCallMediaHealth(in);
  EXPECT_EQ(v.asymmetry, CallAudioAsymmetry::None);
  EXPECT_EQ(v.quality, CallPathQuality::Excellent);
}

TEST(CallMediaHealthTest, NoCaptureDoesNotFlagReceivingOnly) {
  auto in = BaseHealthyInput();
  in.engine.tx_audio_frames = 0;
  in.engine.last_tx_audio_ms = 0;
  in.engine.muted = false;
  in.engine.capture_available = false;
  const auto v = EvaluateCallMediaHealth(in);
  EXPECT_EQ(v.asymmetry, CallAudioAsymmetry::None);
  EXPECT_EQ(v.quality, CallPathQuality::Excellent);
}

TEST(CallMediaHealthTest, DebugSubtitleCapsStaleRxAge) {
  auto in = BaseHealthyInput(100'000);
  in.engine.last_rx_audio_ms = in.now_ms - 60'000;
  const auto v = EvaluateCallMediaHealth(in);
  const std::string sub = FormatCallDebugSubtitle(v, in.now_ms);
  EXPECT_NE(sub.find("rx9999ms"), std::string::npos) << sub;
}

TEST(CallMediaHealthTest, ReconnectingTakesPriority) {
  auto in = BaseHealthyInput();
  in.reconnecting = true;
  const auto v = EvaluateCallMediaHealth(in);
  EXPECT_EQ(v.quality, CallPathQuality::Reconnecting);
  EXPECT_EQ(v.quality_bars, 1);
}

TEST(CallMediaHealthTest, PathKindMediaRelayWhenHopAttached) {
  auto in = BaseHealthyInput();
  in.engine.packet_mode = true;
  // 1:1 Amp sets packet_mode without hop attach — must not force media_relay.
  EXPECT_EQ(EvaluateCallMediaHealth(in).path_kind, "direct");
  in.hop.attached = true;
  const auto v = EvaluateCallMediaHealth(in);
  EXPECT_EQ(v.path_kind, "media_relay");
  EXPECT_NE(FormatCallDebugSubtitle(v, in.now_ms).find("media_relay"), std::string::npos);
}

TEST(CallMediaHealthTest, PathKindHonorsReachModes) {
  auto in = BaseHealthyInput();
  in.engine.packet_mode = true; // 1:1 capture mode — reach label still wins
  in.reach_path_kind = "circuit";
  EXPECT_EQ(EvaluateCallMediaHealth(in).path_kind, "circuit");
  in.reach_path_kind = "punched";
  EXPECT_EQ(EvaluateCallMediaHealth(in).path_kind, "punched");
  in.reach_path_kind = "direct";
  EXPECT_EQ(EvaluateCallMediaHealth(in).path_kind, "direct");
  in.hop.attached = true;
  in.reach_path_kind = "circuit";
  EXPECT_EQ(EvaluateCallMediaHealth(in).path_kind, "media_relay");
}

TEST(CallMediaHealthTest, DiagnosticsGate) {
  SetCallDiagnosticsCliOverride(false);
  EXPECT_FALSE(CallDiagnosticsEnabled(false));
  EXPECT_TRUE(CallDiagnosticsEnabled(true));
  SetCallDiagnosticsCliOverride(true);
  EXPECT_TRUE(CallDiagnosticsEnabled(false));
  SetCallDiagnosticsCliOverride(false);
}

TEST(CallMediaHealthTest, FormatDetailsIncludesDebugSection) {
  const auto v = EvaluateCallMediaHealth(BaseHealthyInput());
  CallDetailsCopy copy;
  copy.elapsed = "1:05";
  copy.path_label = "Direct";
  copy.quality_label = "Good";
  copy.mic_label = "Speaking";
  copy.incoming_label = "Quiet";
  copy.call_id = "c1";
  const std::string thin = FormatCallDetailsText(v, 10'000, false, copy);
  EXPECT_NE(thin.find("Duration: 1:05"), std::string::npos);
  EXPECT_EQ(thin.find("Diagnostics"), std::string::npos);
  const std::string rich = FormatCallDetailsText(v, 10'000, true, copy);
  EXPECT_NE(rich.find("Diagnostics"), std::string::npos);
  EXPECT_NE(rich.find("call_id: c1"), std::string::npos);
}

TEST(CallMediaHealthTest, LogLineHasMediaHealthPrefix) {
  const auto v = EvaluateCallMediaHealth(BaseHealthyInput());
  const std::string line = FormatMediaHealthLogLine(v, 10'000, "abc");
  EXPECT_EQ(line.rfind("media_health ", 0), 0u);
  EXPECT_NE(line.find("call=abc"), std::string::npos);
}

TEST(CallMediaHealthTest, LogLineCarriesFecCounter) {
  CallMediaHealthView v;
  v.engine.plc_frames = 4;
  v.engine.fec_frames = 7;
  const std::string line = FormatMediaHealthLogLine(v, /*now_ms=*/1000, "call:x");
  EXPECT_NE(line.find(" plc=4 fec=7 audio_io=none io_underrun=0 "), std::string::npos) << line;
  EXPECT_NE(line.find(" audio_io="), std::string::npos) << line;
  EXPECT_NE(line.find(" io_underrun="), std::string::npos) << line;
}

// B225: where the video thread is and how long it has been inside one frame fetch, at a glance.
TEST(CallMediaHealthTest, LogLineCarriesTheVideoThreadStage) {
  CallMediaHealthView v;
  v.engine.video_stage = "wait_frame";
  v.engine.video_stage_ms = 4200;
  v.engine.video_fetch_ms = 4100;
  const std::string line = FormatMediaHealthLogLine(v, /*now_ms=*/1000, "call:x");
  EXPECT_NE(line.find(" vstage=wait_frame vstage_ms=4200 vfetch_ms=4100 "), std::string::npos) << line;
}

// #605: a stream whose every packet comes after its slot shows in the log at a glance.
TEST(CallMediaHealthTest, LogLineCarriesLateDrops) {
  CallMediaHealthView v;
  v.engine.jitter_speech_drops = 2;
  v.engine.jitter_late_drops = 9;
  const std::string line = FormatMediaHealthLogLine(v, /*now_ms=*/1000, "call:x");
  EXPECT_NE(line.find(" jb_speech_drops=2 jb_late_drops=9 "), std::string::npos) << line;
}

TEST(CallMediaHealthTest, LogLineIncludesHopPeers) {
  auto in = BaseHealthyInput();
  CallHopPeerHealth peer;
  peer.peer_id = "12D3KooWTestPeerABCDEFGH";
  peer.bytes_up = 100;
  peer.bytes_down = 200;
  peer.drops_queue = 3;
  peer.outbound_backlog = 1;
  in.hop.peers.push_back(peer);
  const auto v = EvaluateCallMediaHealth(in);
  const std::string line = FormatMediaHealthLogLine(v, 10'000, "abc");
  EXPECT_NE(line.find("hop_peers="), std::string::npos);
  EXPECT_NE(line.find("ABCDEFGH:up=100/dn=200/dq=3/bl=1"), std::string::npos);
}

TEST(CallMediaHealthTest, LinkHealthRoundTripAndResendsBetweenSamples) {
  CallLinkCounters before{.available = true, .reliable_sent = 100, .retransmits = 2, .srtt_ms = 40};
  CallLinkCounters now{.available = true, .reliable_sent = 150, .retransmits = 7, .srtt_ms = 55};
  const CallLinkHealth health = CallLinkHealthBetween(before, now);
  EXPECT_TRUE(health.available);
  EXPECT_EQ(health.rtt_ms, 55); // the smoothed figure now, not a delta
  EXPECT_DOUBLE_EQ(health.resend_pct, 10.0);
}

TEST(CallMediaHealthTest, LinkHealthHasNoResendRateWithoutTwoSamplesOfOneAssociation) {
  const CallLinkCounters now{.available = true, .reliable_sent = 10, .retransmits = 1, .srtt_ms = 30};
  EXPECT_LT(CallLinkHealthBetween(CallLinkCounters{}, now).resend_pct, 0.0) << "first sample";
  // A path move starts a new association: its counters restart below the last sample.
  const CallLinkCounters old{.available = true, .reliable_sent = 500, .retransmits = 50, .srtt_ms = 80};
  const CallLinkHealth moved = CallLinkHealthBetween(old, now);
  EXPECT_TRUE(moved.available);
  EXPECT_EQ(moved.rtt_ms, 30);
  EXPECT_LT(moved.resend_pct, 0.0);
  EXPECT_FALSE(CallLinkHealthBetween(now, CallLinkCounters{}).available);
}

// Review: the call switches links (hop attach, a move onto a long-lived relay link) — the counters of
// another link are not a delta, even when they happen to be larger.
TEST(CallMediaHealthTest, LinkHealthHasNoResendRateAcrossALinkSwitch) {
  const CallLinkCounters direct{.available = true, .link_id = 7, .reliable_sent = 100, .retransmits = 0, .srtt_ms = 20};
  const CallLinkCounters relay{.available = true, .link_id = 9, .reliable_sent = 101, .retransmits = 50, .srtt_ms = 60};
  const CallLinkHealth health = CallLinkHealthBetween(direct, relay);
  EXPECT_TRUE(health.available);
  EXPECT_EQ(health.rtt_ms, 60);
  EXPECT_LT(health.resend_pct, 0.0) << "not 5000%";
}

TEST(CallMediaHealthTest, LinkHealthResendRateIsCappedAtEveryPacket) {
  const CallLinkCounters before{.available = true, .link_id = 7, .reliable_sent = 100, .retransmits = 0, .srtt_ms = 20};
  const CallLinkCounters now{.available = true, .link_id = 7, .reliable_sent = 102, .retransmits = 9, .srtt_ms = 20};
  EXPECT_DOUBLE_EQ(CallLinkHealthBetween(before, now).resend_pct, 100.0);
}

TEST(CallMediaHealthTest, LinkHealthInformsButDoesNotGrade) {
  CallMediaHealthInput in;
  in.engine.active = true;
  in.now_ms = 10000;
  in.engine.last_rx_audio_ms = 9900;
  in.engine.last_tx_audio_ms = 9900;
  in.engine.rx_audio_frames = 100;
  in.engine.tx_audio_frames = 100;
  const CallPathQuality baseline = EvaluateCallMediaHealth(in).quality;
  in.link = CallLinkHealth{.available = true, .rtt_ms = 900, .resend_pct = 40.0};
  const CallMediaHealthView view = EvaluateCallMediaHealth(in);
  EXPECT_EQ(view.quality, baseline);
  EXPECT_EQ(view.link.rtt_ms, 900);
}

TEST(CallMediaHealthTest, DetailsShowTheNetworkLineOnlyWhenGiven) {
  CallMediaHealthView view;
  CallDetailsCopy copy;
  EXPECT_EQ(FormatCallDetailsText(view, 0, false, copy).find("Network:"), std::string::npos);
  copy.network_label = "Round trip 42 ms";
  EXPECT_NE(FormatCallDetailsText(view, 0, false, copy).find("Network: Round trip 42 ms"), std::string::npos);
}

// --- B231: buffer pressure right after a local route change (the speaker toggle) is not a weak signal.

/** One evaluation as CallMediaFrame does it: the gate's pressure grades, the engine's figure stays for the log. */
CallMediaHealthView EvaluateGated(CallBufferPressureGate& gate, CallMediaHealthInput in,
                                  const int64_t route_changed_ms) {
  in.engine_pressure_for_quality = gate.Step(in.engine.path_pressure, in.now_ms, route_changed_ms);
  return EvaluateCallMediaHealth(in);
}

CallMediaHealthInput PressureAt(const int64_t now_ms, const double pressure) {
  auto in = BaseHealthyInput(now_ms);
  in.engine.path_pressure = pressure;
  return in;
}

TEST(CallBufferPressureGateTest, ASpikeRightAfterARouteChangeIsNotPoor) {
  CallBufferPressureGate gate;
  const int64_t toggled = 10'000;
  for (int64_t t = toggled; t < toggled + kCallRouteChangeSettleMs; t += 100) {
    const auto v = EvaluateGated(gate, PressureAt(t, 1.0), toggled);
    EXPECT_EQ(v.quality, CallPathQuality::Excellent) << "at +" << (t - toggled) << " ms";
    EXPECT_EQ(v.engine.path_pressure, 1.0) << "the media_health line still shows the engine's pressure";
  }
}

TEST(CallBufferPressureGateTest, SustainedPressureWithoutARouteChangeIsPoorAfterASecond) {
  CallBufferPressureGate gate;
  const int64_t from = 10'000;
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from, 1.0), 0).quality, CallPathQuality::Fair);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + 500, 1.0), 0).quality, CallPathQuality::Fair);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + 999, 1.0), 0).quality, CallPathQuality::Fair);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + kCallBufferPressureSustainMs, 1.0), 0).quality,
            CallPathQuality::Poor);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + 1'500, 1.0), 0).quality, CallPathQuality::Poor);
}

TEST(CallBufferPressureGateTest, ADipStartsTheSecondOver) {
  CallBufferPressureGate gate;
  const int64_t from = 10'000;
  (void)EvaluateGated(gate, PressureAt(from, 1.0), 0);
  (void)EvaluateGated(gate, PressureAt(from + 800, 1.0), 0);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + 900, 0.2), 0).quality, CallPathQuality::Good);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + 1'000, 1.0), 0).quality, CallPathQuality::Fair);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + 1'500, 1.0), 0).quality, CallPathQuality::Fair);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + 2'000, 1.0), 0).quality, CallPathQuality::Poor);
}

TEST(CallBufferPressureGateTest, PressureOutlastingTheSettleTimeIsTimedFromItsEnd) {
  CallBufferPressureGate gate;
  const int64_t toggled = 10'000;
  const int64_t settled = toggled + kCallRouteChangeSettleMs;
  EXPECT_EQ(EvaluateGated(gate, PressureAt(settled - 100, 1.0), toggled).quality, CallPathQuality::Excellent);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(settled, 1.0), toggled).quality, CallPathQuality::Fair);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(settled + 900, 1.0), toggled).quality, CallPathQuality::Fair);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(settled + 1'000, 1.0), toggled).quality, CallPathQuality::Poor);
}

// A route that flaps every second (a Bluetooth link, say) must not hide a call that is really degrading: the first
// change opens one window, the changes inside it and in the cooldown after it open none.
TEST(CallBufferPressureGateTest, ARouteFlappingEverySecondDoesNotHideSustainedPressure) {
  CallBufferPressureGate gate;
  const int64_t from = 10'000;
  int64_t poor_at = -1;
  for (int64_t t = from; t <= from + 20'000; t += 100) {
    const int64_t flap = from + (t - from) / 1000 * 1000; // the newest change, once a second
    const auto v = EvaluateGated(gate, PressureAt(t, 1.0), flap);
    if (v.quality == CallPathQuality::Poor && poor_at < 0) {
      poor_at = t;
    }
    if (poor_at >= 0) {
      EXPECT_EQ(v.quality, CallPathQuality::Poor) << "stays Poor, at +" << (t - from) << " ms";
    }
  }
  ASSERT_GE(poor_at, 0);
  EXPECT_LE(poor_at - from, 4'000) << "Poor within about 4 s";
}

TEST(CallBufferPressureGateTest, AChangeInsideTheWindowDoesNotRestartIt) {
  CallBufferPressureGate gate;
  const int64_t from = 10'000;
  (void)EvaluateGated(gate, PressureAt(from, 1.0), from);
  (void)EvaluateGated(gate, PressureAt(from + 2'000, 1.0), from + 2'000); // inside: ignored
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + kCallRouteChangeSettleMs, 1.0), from + 2'000).quality,
            CallPathQuality::Fair)
      << "the window ended at +3 s, not +5 s";
}

TEST(CallBufferPressureGateTest, AnotherWindowOpensOnlyTenSecondsAfterTheLastEnded) {
  CallBufferPressureGate gate;
  const int64_t from = 10'000;
  const int64_t ended = from + kCallRouteChangeSettleMs;
  (void)EvaluateGated(gate, PressureAt(from, 1.0), from);
  // A second change 5 s after the first window ended: in the cooldown, so it hides nothing.
  int64_t second = ended + 5'000;
  for (int64_t t = ended; t <= second + 100; t += 100) {
    (void)EvaluateGated(gate, PressureAt(t, 1.0), t >= second ? second : from);
  }
  EXPECT_EQ(EvaluateGated(gate, PressureAt(second + 200, 1.0), second).quality, CallPathQuality::Poor);
  // A third one after the cooldown opens a window: a healthy call shows no flash.
  CallBufferPressureGate healthy;
  (void)EvaluateGated(healthy, PressureAt(from, 0.0), from);
  for (int64_t t = ended; t < ended + kCallRouteChangeCooldownMs; t += 500) {
    (void)EvaluateGated(healthy, PressureAt(t, 0.0), from);
  }
  const int64_t third = ended + kCallRouteChangeCooldownMs;
  EXPECT_EQ(EvaluateGated(healthy, PressureAt(third, 1.0), third).quality, CallPathQuality::Excellent);
}

TEST(CallBufferPressureGateTest, AChangeThatWasLongAgoWhenFirstSeenOpensNoWindow) {
  CallBufferPressureGate gate;
  EXPECT_EQ(EvaluateGated(gate, PressureAt(20'000, 1.0), 10'000).quality, CallPathQuality::Fair)
      << "a stale change (a previous call's) is not a window";
}

TEST(CallBufferPressureGateTest, APauseInEvaluationStartsThePersistenceOver) {
  CallBufferPressureGate gate;
  const int64_t from = 10'000;
  (void)EvaluateGated(gate, PressureAt(from, 1.0), 0);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + 1'000, 1.0), 0).quality, CallPathQuality::Poor);
  // Nothing evaluated for 5 s (a ring over the call, the app in the background), then the same pressure.
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + 6'000, 1.0), 0).quality, CallPathQuality::Fair);
  EXPECT_EQ(EvaluateGated(gate, PressureAt(from + 7'000, 1.0), 0).quality, CallPathQuality::Poor);
}

TEST(CallBufferPressureGateTest, ModeratePressureGradesAtOnce) {
  CallBufferPressureGate gate;
  EXPECT_EQ(EvaluateGated(gate, PressureAt(10'000, 0.5), 0).quality, CallPathQuality::Fair);
}

TEST(CallBufferPressureGateTest, LossRightAfterARouteChangeStillDropsTheBars) {
  const int64_t toggled = 10'000;
  const int64_t now = toggled + 300;
  {
    CallBufferPressureGate gate;
    auto in = PressureAt(now, 1.0);
    in.engine.tx_audio_frames = 0; // nothing sent (not muted, mic present)
    in.engine.last_tx_audio_ms = 0;
    const auto v = EvaluateGated(gate, in, toggled);
    EXPECT_EQ(v.quality, CallPathQuality::Poor);
    EXPECT_EQ(v.asymmetry, CallAudioAsymmetry::ReceivingOnly);
  }
  {
    CallBufferPressureGate gate;
    auto in = PressureAt(now, 1.0);
    in.engine.last_rx_audio_ms = now - 5'000; // nothing heard for 5 s
    EXPECT_EQ(EvaluateGated(gate, in, toggled).quality, CallPathQuality::NoAudio);
  }
  {
    CallBufferPressureGate gate;
    auto in = PressureAt(now, 1.0);
    in.hop.path_pressure = 0.9; // the relay's pressure is the network's
    EXPECT_EQ(EvaluateGated(gate, in, toggled).quality, CallPathQuality::Poor);
  }
  {
    CallBufferPressureGate gate;
    auto in = PressureAt(now, 1.0);
    in.reconnecting = true;
    EXPECT_EQ(EvaluateGated(gate, in, toggled).quality, CallPathQuality::Reconnecting);
  }
}

} // namespace
} // namespace pbr
