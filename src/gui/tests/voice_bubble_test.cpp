#include "feature/conversations/InboxController.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/chat/VoiceBubble.h"
#include "lib/messaging/ChatPayloadCodec.h"
#include "lib/messaging/TranscriptAssembly.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

TranscriptRow Attachment(const std::string& id, const std::string& name, const std::string& mime, const uint64_t bytes) {
  ChatAttachmentFields fields;
  fields.mime = mime;
  fields.filename = name;
  fields.byte_length = bytes;
  fields.url = "https://example.invalid/blob";
  fields.content_hash.assign(32, 1);
  fields.content_key.assign(32, 2);
  fields.blob_nonce.assign(24, 3);
  TranscriptRow row;
  row.message.id = id;
  row.message.thread_id = "t";
  row.message.sender_contact_id = "contact-1";
  row.message.content_type = ChatContentType::Attachment;
  row.message.payload_json = ChatPayloadCodec::AttachmentFieldsToJson(fields);
  row.message.timestamp = 1;
  return row;
}

TranscriptFacts Facts(const bool supported, const AttachmentFetchWorkflow::DownloadState state =
                                                AttachmentFetchWorkflow::DownloadState::Ready) {
  TranscriptFacts facts;
  facts.attachment = [state](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = state;
    view.local_path = "/tmp/x";
    return view;
  };
  facts.voice_supported = supported;
  facts.voice_max_seconds = 120;
  return facts;
}

std::string Render(const TranscriptRow& row, const TranscriptFacts& facts) {
  const auto rows = RenderTranscript({row}, facts);
  return rows.empty() ? std::string() : std::string(rows[0].content_rml.c_str());
}

bool IsBubble(const std::string& rml) { return rml.find("chat-voice-card") != std::string::npos; }

TEST(VoiceBubbleRender, SmallMatchingFileIsABubbleWithItsLength) {
  const std::string rml = Render(Attachment("m1", "voice-4200.ogg", "audio/ogg", 25000), Facts(true));
  EXPECT_TRUE(IsBubble(rml)) << rml;
  EXPECT_NE(rml.find("play_voice('m1')"), std::string::npos);
  EXPECT_NE(rml.find("0:05"), std::string::npos);                    // 4.2 s reads as 0:05
  EXPECT_NE(rml.find("id=\"vf-m1\""), std::string::npos);
  EXPECT_EQ(rml.find("chat-attachment-card"), std::string::npos);
}

TEST(VoiceBubbleRender, AFileRowForLargeDeclaredSizeOtherNameOtherTypeOrNoPlayer) {
  EXPECT_FALSE(IsBubble(Render(Attachment("m", "voice-4200.ogg", "audio/ogg", 1000001), Facts(true))));
  EXPECT_FALSE(IsBubble(Render(Attachment("m", "song.ogg", "audio/ogg", 25000), Facts(true))));
  EXPECT_FALSE(IsBubble(Render(Attachment("m", "voice-4200.ogg", "audio/mpeg", 25000), Facts(true))));
  EXPECT_FALSE(IsBubble(Render(Attachment("m", "voice-4200.ogg", "audio/ogg", 25000), Facts(false)))); // Linux
  const std::string row = Render(Attachment("m", "voice-4200.ogg", "audio/ogg", 1000001), Facts(true));
  EXPECT_NE(row.find("voice-4200.ogg"), std::string::npos); // the file row names the file
}

TEST(VoiceBubbleRender, NothingToPlayUntilItHasBeenDownloaded) {
  const auto pending = Facts(true, AttachmentFetchWorkflow::DownloadState::Pending);
  EXPECT_FALSE(IsBubble(Render(Attachment("m", "voice-4200.ogg", "audio/ogg", 25000), pending)));
}

TEST(VoiceBubbleRender, ShownLengthNeverPassesTheVoiceLimit) {
  const std::string rml = Render(Attachment("m", "voice-999999999.ogg", "audio/ogg", 25000), Facts(true));
  EXPECT_TRUE(IsBubble(rml));
  EXPECT_NE(rml.find("2:00"), std::string::npos);
  EXPECT_EQ(rml.find("277777"), std::string::npos);
}

TEST(VoiceBubbleRender, ARedrawKeepsTheProgressOfThePlayingBubble) {
  auto facts = Facts(true);
  facts.voice_state = [](const std::string& id) {
    VoiceBubbleState state;
    if (id == "m1") {
      state.playing = true;
      state.position_ms = 2000;
      state.duration_ms = 8000;
    }
    return state;
  };
  const std::string playing = Render(Attachment("m1", "voice-8000.ogg", "audio/ogg", 30000), facts);
  EXPECT_NE(playing.find("chat-voice-play--playing"), std::string::npos);
  EXPECT_NE(playing.find("width: 25%"), std::string::npos);
  EXPECT_NE(playing.find("0:02 / 0:08"), std::string::npos);
  const std::string other = Render(Attachment("m2", "voice-8000.ogg", "audio/ogg", 30000), facts);
  EXPECT_EQ(other.find("chat-voice-play--playing"), std::string::npos);
  EXPECT_NE(other.find("width: 0%"), std::string::npos);
}

TEST(VoiceBubbleRender, ElementKeysAreSafeInMarkup) {
  EXPECT_EQ(VoiceElementKey("a1-B_2"), "a1-B_2");
  EXPECT_EQ(VoiceElementKey("a\"b c<d>'"), "a_b_c_d__");
}

TEST(VoiceBubbleRender, TimeLineAndFill) {
  VoiceBubbleState idle;
  EXPECT_EQ(VoiceBubbleTimeText(idle, 7000), "0:07");
  EXPECT_EQ(VoiceBubbleFillPercent(idle), 0);
  VoiceBubbleState paused;
  paused.paused = true;
  paused.position_ms = 65000;
  paused.duration_ms = 120000;
  EXPECT_EQ(VoiceBubbleTimeText(paused, 120000), "1:05 / 2:00");
  EXPECT_EQ(VoiceBubbleFillPercent(paused), 54);
}

} // namespace
} // namespace pbr
