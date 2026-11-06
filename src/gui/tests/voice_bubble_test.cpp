#include "feature/conversations/InboxController.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/chat/VoiceBubble.h"
#include "lib/delivery/ChatPayloadCodec.h"
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

// #111: an MP3 sent as a file plays in the chat (iOS QuickLook showed only an info page): its name, a play button,
// a bar and the time line, with the voice ids VoicePlayback updates; the click is the file player's.
TEST(VoiceBubbleRender, ASoundFilePlaysInPlaceWhereThePlatformHasAPlayer) {
  TranscriptFacts facts = Facts(true);
  facts.audio_files_play = true;
  const std::string rml = Render(Attachment("m7", "Song.mp3", "audio/mpeg", 10800000), facts);
  EXPECT_NE(rml.find("chat-audio-file-card"), std::string::npos) << rml;
  EXPECT_NE(rml.find("play_audio_file('m7')"), std::string::npos);
  EXPECT_NE(rml.find("id=\"vp-m7\""), std::string::npos);
  EXPECT_NE(rml.find("id=\"vf-m7\""), std::string::npos);
  EXPECT_NE(rml.find("id=\"vt-m7\""), std::string::npos);
  EXPECT_NE(rml.find("Song.mp3"), std::string::npos);
  EXPECT_NE(rml.find("10 MB"), std::string::npos) << "before it plays, the time line gives the size";
  EXPECT_EQ(rml.find("open_attachment("), std::string::npos) << "not handed to the system's viewer";
  // An m4a named file with a generic type plays too; a PDF stays a file row with Open.
  EXPECT_NE(Render(Attachment("m8", "a.m4a", "application/octet-stream", 5000), facts).find("play_audio_file"),
            std::string::npos);
  EXPECT_EQ(Render(Attachment("m9", "doc.pdf", "application/pdf", 5000), facts).find("play_audio_file"),
            std::string::npos);
  // A voice message keeps its own bubble.
  EXPECT_NE(Render(Attachment("m1", "voice-4200.ogg", "audio/ogg", 25000), facts).find("play_voice('m1')"),
            std::string::npos);
}

TEST(VoiceBubbleRender, WithoutAPlayerASoundFileIsAFileRowToOpen) {
  const std::string rml = Render(Attachment("m7", "Song.mp3", "audio/mpeg", 10800000), Facts(true));
  EXPECT_EQ(rml.find("play_audio_file"), std::string::npos);
  EXPECT_NE(rml.find("open_attachment('m7')"), std::string::npos);
}

TEST(VoiceBubbleRender, APlayingSoundFileKeepsItsProgressOnARedraw) {
  const VoiceBubbleState state{.playing = true, .paused = false, .position_ms = 65000, .duration_ms = 130000};
  const std::string rml = AudioFileBubbleRml("m7", "m7", "Song.mp3", "10 MB", state);
  EXPECT_NE(rml.find("chat-voice-play--playing"), std::string::npos);
  EXPECT_NE(rml.find("width: 50%"), std::string::npos);
  EXPECT_NE(rml.find("1:05 / 2:10"), std::string::npos);
}

// Kenneth 2026-11-03: a blurry picture in the chat could not be enlarged — tapping it did nothing.
TEST(VoiceBubbleRender, TappingAPictureOpensItFull) {
  const std::string rml = Render(Attachment("p1", "photo.jpg", "image/jpeg", 50000), Facts(true));
  EXPECT_NE(rml.find("chat-attachment-image"), std::string::npos) << rml;
  EXPECT_NE(rml.find("data-event-click=\"open_picture('p1','"), std::string::npos) << rml;
}

// Kenneth 2026-11-03: picture bubbles had one fixed size (a portrait picture filled two thirds, the rest was empty). The
// picture card carries its own modifier, so on phones it shrinks to the picture's shape; a file card does not.
TEST(VoiceBubbleRender, APictureCardIsMarkedAndKeepsItsTailAndReactions) {
  TranscriptRow picture = Attachment("p2", "photo.png", "image/png", 50000);
  picture.reactions.push_back(TranscriptReaction{"\xF0\x9F\x91\x8D", 1, false});
  const std::string rml = Render(picture, Facts(true));
  EXPECT_EQ(rml.rfind("<div class=\"chat-card chat-attachment-card chat-attachment-card--image", 0), 0u) << rml;
  EXPECT_NE(rml.find("bubble-tail-in"), std::string::npos) << "the last bubble of a run keeps its tail";
  EXPECT_NE(rml.find("chat-reaction-row--overlay"), std::string::npos) << rml;
  const std::string file = Render(Attachment("f1", "notes.pdf", "application/pdf", 50000), Facts(true));
  EXPECT_EQ(file.find("chat-attachment-card--image"), std::string::npos) << file;
  EXPECT_NE(file.find("chat-attachment-card"), std::string::npos) << file;
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

// A large picture whose thumbnail is ready (the view carries the thumbnail path, on_demand set) is drawn as an inline
// image, not a file card.
TEST(VoiceBubbleRender, ALargePictureWithAThumbnailIsAnInlineImage) {
  TranscriptFacts facts;
  facts.attachment = [](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.on_demand = true;
    view.local_path = "/tmp/thumb.poster.jpg";
    view.inline_image_ok = true;
    return view;
  };
  const std::string rml = Render(Attachment("m9", "big.jpg", "image/jpeg", 6u * 1024 * 1024), facts);
  EXPECT_NE(rml.find("chat-attachment-image"), std::string::npos) << rml;
  EXPECT_NE(rml.find("/tmp/thumb.poster.jpg"), std::string::npos);
}

// Kenneth 2026-11-04 (PLAN-ui-refresh-impl 62.3): pictures spilled out of their bubble. Every shape fits a
// 200 x 240dp box with its own ratio, and the picture carries that size so its 3dp frame is laid out round it.
TEST(PictureBubbleFit, EveryShapeFitsTheBoxWithItsOwnRatio) {
  const auto fit = [](const int w, const int h) {
    const PictureBubbleSize size = FitPictureInBubble(w, h);
    return std::make_pair(size.width_dp, size.height_dp);
  };
  EXPECT_EQ(fit(4032, 3024), std::make_pair(200, 150)) << "a landscape photo";
  EXPECT_EQ(fit(3024, 4032), std::make_pair(180, 240)) << "a portrait photo";
  EXPECT_EQ(fit(600, 2400), std::make_pair(60, 240)) << "very tall";
  EXPECT_EQ(fit(2400, 400), std::make_pair(200, 33)) << "very wide";
  EXPECT_EQ(fit(20000, 10), std::make_pair(200, 1)) << "a sliver keeps 1dp";
  EXPECT_EQ(fit(120, 90), std::make_pair(120, 90)) << "a small picture is not enlarged";
  EXPECT_EQ(fit(0, 300), std::make_pair(0, 0)) << "unknown size";
}

TEST(PictureBubbleFit, ThePictureCarriesItsFittedSize) {
  TranscriptFacts facts;
  facts.attachment = [](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.local_path = "/tmp/tall.jpg";
    view.image_width = 3024;  // upright: InboxController has swapped a quarter-turned photo's sides
    view.image_height = 4032;
    return view;
  };
  const std::string rml = Render(Attachment("p3", "tall.jpg", "image/jpeg", 50000), facts);
  EXPECT_NE(rml.find("width=\"180\" height=\"240\""), std::string::npos) << rml;
  // The picture sits in a same-sized clip box that rounds its corners (the engine does not round an <img>).
  EXPECT_NE(rml.find("<div class=\"chat-attachment-clip\" style=\"width:180dp;height:240dp\"><img "),
            std::string::npos)
      << rml;
  EXPECT_NE(rml.find("')\"/></div>"), std::string::npos) << rml;
  // Without a known size the picture is left to the stylesheet's limits.
  const std::string unknown = Render(Attachment("p4", "x.jpg", "image/jpeg", 50000), Facts(true));
  EXPECT_NE(unknown.find("class=\"chat-attachment-image\" src=\"/tmp/x\" data-event-click="), std::string::npos)
      << unknown;
}

} // namespace
} // namespace pbr
