#include "base/media/CallMediaHealth.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

CallMediaHealthInput BaseHealthyInput(int64_t now_ms = 10'000) {
  CallMediaHealthInput in;
  in.now_ms = now_ms;
  in.engine.active = true;
  in.engine.connected = true;
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

TEST(CallMediaHealthTest, ReconnectingTakesPriority) {
  auto in = BaseHealthyInput();
  in.reconnecting = true;
  const auto v = EvaluateCallMediaHealth(in);
  EXPECT_EQ(v.quality, CallPathQuality::Reconnecting);
  EXPECT_EQ(v.quality_bars, 1);
}

TEST(CallMediaHealthTest, PathKindRelayWhenSfu) {
  auto in = BaseHealthyInput();
  in.engine.sfu_mode = true;
  const auto v = EvaluateCallMediaHealth(in);
  EXPECT_EQ(v.path_kind, "relay");
  EXPECT_NE(FormatCallDebugSubtitle(v, in.now_ms).find("SFU"), std::string::npos);
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

} // namespace
} // namespace pbr
