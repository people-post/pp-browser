#include "feature/conversations/InboxController.h"
#include "gui/chat/TranscriptRml.h"
#include "gui/chat/VoiceBubble.h"
#include "lib/delivery/ChatPayloadCodec.h"
#include "lib/messaging/TranscriptAssembly.h"

#include <gtest/gtest.h>

#include <algorithm>

namespace pbr {
namespace {

TranscriptRow Attachment(const std::string& id, const std::string& name, const std::string& mime,
                         const uint64_t bytes) {
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

bool IsBubble(const std::string& rml) {
  return rml.find("chat-voice-card") != std::string::npos;
}

TEST(VoiceBubbleRender, SmallMatchingFileIsABubbleWithItsLength) {
  const std::string rml = Render(Attachment("m1", "voice-4200.ogg", "audio/ogg", 25000), Facts(true));
  EXPECT_TRUE(IsBubble(rml)) << rml;
  EXPECT_NE(rml.find("play_voice('m1')"), std::string::npos);
  EXPECT_NE(rml.find("0:05"), std::string::npos); // 4.2 s reads as 0:05
  EXPECT_NE(rml.find("id=\"vf-m1\""), std::string::npos);
  EXPECT_EQ(rml.find("chat-attachment-card"), std::string::npos);
}

// #201: a voice message looks like WhatsApp's: the sender's avatar with a mic badge, play, a waveform of thin bars
// with a knob, the length under it and the clock; a bubble of its own width, not the row's.
TEST(VoiceBubbleRender, TheBubbleHasAvatarBadgePlayWaveformLengthAndClock) {
  TranscriptRow row = Attachment("m1", "voice-4200.ogg", "audio/ogg", 25000);
  row.message.timestamp = 1700000000000;
  TranscriptFacts facts = Facts(true);
  facts.sender_label = [](const std::string&) { return std::string("Bob"); };
  const std::string rml = Render(row, facts);
  ASSERT_TRUE(IsBubble(rml)) << rml;
  EXPECT_NE(rml.find("class=\"chat-voice-avatar avatar-tone-"), std::string::npos);
  EXPECT_NE(rml.find("chat-voice-avatar-letter\">B<"), std::string::npos) << "the letter avatar of the sender";
  EXPECT_NE(rml.find("class=\"chat-voice-badge\""), std::string::npos);
  EXPECT_NE(rml.find("icons/v2/mic.svg"), std::string::npos);
  EXPECT_NE(rml.find("id=\"vp-m1\" class=\"chat-voice-play"), std::string::npos);
  EXPECT_LT(rml.find("chat-voice-avatar"), rml.find("id=\"vp-m1\""));
  EXPECT_LT(rml.find("id=\"vp-m1\""), rml.find("chat-voice-wave"));
  // The bars are drawn twice (quiet layer and played layer), kVoiceWaveBars each.
  size_t bars = 0;
  for (size_t at = rml.find("class=\"chat-voice-bar\""); at != std::string::npos;
       at = rml.find("class=\"chat-voice-bar\"", at + 1)) {
    ++bars;
  }
  EXPECT_EQ(bars, static_cast<size_t>(2 * kVoiceWaveBars));
  EXPECT_NE(rml.find("id=\"vn-m1\" class=\"chat-voice-knob\" style=\"left: 0%;\""), std::string::npos);
  EXPECT_LT(rml.find("chat-voice-wave"), rml.find("id=\"vt-m1\""));
  EXPECT_NE(rml.find("class=\"chat-voice-clock\""), std::string::npos);
  EXPECT_LT(rml.find("id=\"vt-m1\""), rml.find("class=\"chat-voice-clock\""));
  EXPECT_EQ(rml.find("chat-audio-seek"), std::string::npos) << "a voice message has no seek bar";
}

TEST(VoiceBubbleRender, OwnVoiceBubbleIsMarkedAndAPhotoReplacesTheLetter) {
  VoiceBubbleFace face;
  face.ours = true;
  face.icon_path = "/profiles/x/icons/a.jpg";
  const std::string rml = VoiceBubbleRml("m2", "m2", 4200, VoiceBubbleState{}, face);
  EXPECT_NE(rml.find("chat-voice-card chat-voice-card--out"), std::string::npos);
  EXPECT_NE(rml.find("src=\"/profiles/x/icons/a.jpg\""), std::string::npos);
  EXPECT_EQ(rml.find("chat-voice-avatar-letter"), std::string::npos);
  EXPECT_EQ(rml.find("chat-voice-clock"), std::string::npos) << "no clock given: none drawn";
}

TEST(VoiceBubbleRender, TheWaveformIsTheSameForTheSameMessageAndVaries) {
  const std::vector<int> a = VoiceWaveformHeights("msg-1");
  EXPECT_EQ(a, VoiceWaveformHeights("msg-1"));
  EXPECT_NE(a, VoiceWaveformHeights("msg-2"));
  ASSERT_EQ(a.size(), static_cast<size_t>(kVoiceWaveBars));
  for (const int h : a) {
    EXPECT_GE(h, 4);
    EXPECT_LE(h, 28);
  }
  EXPECT_GT(*std::max_element(a.begin(), a.end()) - *std::min_element(a.begin(), a.end()), 8) << "not a flat line";
  EXPECT_TRUE(VoiceWaveformHeights("x", 0).empty());
  // A redraw keeps the progress and the same bars.
  VoiceBubbleState playing;
  playing.playing = true;
  playing.position_ms = 1000;
  playing.duration_ms = 4000;
  const std::string one = VoiceBubbleRml("m3", "m3", 4000, playing, VoiceBubbleFace{});
  EXPECT_NE(one.find("id=\"vf-m3\" class=\"chat-voice-played\" style=\"width: 25%;\""), std::string::npos) << one;
  EXPECT_NE(one.find("style=\"left: 25%;\""), std::string::npos);
  EXPECT_EQ(one, VoiceBubbleRml("m3", "m3", 4000, playing, VoiceBubbleFace{}));
}

TEST(VoiceBubbleRender, AFileRowForLargeDeclaredSizeOtherNameOtherTypeOrNoPlayer) {
  EXPECT_FALSE(IsBubble(Render(Attachment("m", "voice-4200.ogg", "audio/ogg", 1000001), Facts(true))));
  EXPECT_FALSE(IsBubble(Render(Attachment("m", "song.ogg", "audio/ogg", 25000), Facts(true))));
  EXPECT_FALSE(IsBubble(Render(Attachment("m", "voice-4200.ogg", "audio/mpeg", 25000), Facts(true))));
  EXPECT_FALSE(IsBubble(Render(Attachment("m", "voice-4200.ogg", "audio/ogg", 25000), Facts(false)))); // Linux
  const std::string row = Render(Attachment("m", "voice-4200.ogg", "audio/ogg", 1000001), Facts(true));
  EXPECT_NE(row.find("voice-4200.ogg"), std::string::npos); // the file row names the file
}

// #111: an MP3 sent as a file plays in the chat (iOS QuickLook showed only an info page): a music card with its name,
// a play button, a bar and the time line, with the voice ids VoicePlayback updates; the click is the file player's.
TEST(VoiceBubbleRender, ASoundFilePlaysInPlaceWhereThePlatformHasAPlayer) {
  TranscriptFacts facts = Facts(true);
  facts.audio_files_play = true;
  const std::string rml = Render(Attachment("m7", "Song.mp3", "audio/mpeg", 10800000), facts);
  EXPECT_NE(rml.find("chat-audio-card"), std::string::npos) << rml;
  EXPECT_NE(rml.find("play_audio_file('m7')"), std::string::npos);
  EXPECT_NE(rml.find("id=\"vp-m7\""), std::string::npos);
  EXPECT_NE(rml.find("id=\"vf-m7\""), std::string::npos);
  EXPECT_NE(rml.find("id=\"vt-m7\""), std::string::npos);
  EXPECT_NE(rml.find("Song.mp3"), std::string::npos);
  EXPECT_NE(rml.find("10 MB"), std::string::npos) << "the line under the name gives the size";
  EXPECT_NE(rml.find("chat-audio-cover-note"), std::string::npos) << "no tags read: the music-note placeholder";
  EXPECT_EQ(rml.find("open_attachment("), std::string::npos) << "not handed to the system's viewer";
  // The bar sits in a wrapper that takes the press (seek by tap and drag); the thin bar is inside it.
  const size_t seek = rml.find("id=\"vk-m7\" class=\"chat-audio-seek\" data-event-mousedown=\"audio_seek_press()\"");
  ASSERT_NE(seek, std::string::npos) << rml;
  EXPECT_LT(seek, rml.find("class=\"chat-voice-track\"")) << "the track is inside the touch area";
  EXPECT_LT(rml.find("class=\"chat-voice-track\""), rml.find("id=\"vf-m7\""));
  // The drag knob sits in the wrapper beside the (clipping) track, at the fill's end; own / received colours are
  // keyed off the row in the theme.
  const size_t knob = rml.find("id=\"vn-m7\" class=\"chat-audio-knob\" style=\"left: 0%;\"");
  ASSERT_NE(knob, std::string::npos) << rml;
  EXPECT_GT(knob, rml.find("id=\"vf-m7\""));
  EXPECT_LT(knob, rml.find("class=\"chat-voice-time"));
  // An m4a named file with a generic type plays too; a PDF stays a file row with Open.
  EXPECT_NE(Render(Attachment("m8", "a.m4a", "application/octet-stream", 5000), facts).find("play_audio_file"),
            std::string::npos);
  EXPECT_EQ(Render(Attachment("m9", "doc.pdf", "application/pdf", 5000), facts).find("play_audio_file"),
            std::string::npos);
  // A voice message keeps its own bubble.
  EXPECT_NE(Render(Attachment("m1", "voice-4200.ogg", "audio/ogg", 25000), facts).find("play_voice('m1')"),
            std::string::npos);
}

TEST(VoiceBubbleRender, ThePointerSpotInTheBarIsAFractionClampedToIt) {
  EXPECT_DOUBLE_EQ(SeekFractionAt(150.f, 100.f, 200.f), 0.25);
  EXPECT_DOUBLE_EQ(SeekFractionAt(50.f, 100.f, 200.f), 0.0) << "left of the bar";
  EXPECT_DOUBLE_EQ(SeekFractionAt(900.f, 100.f, 200.f), 1.0) << "right of the bar";
  EXPECT_DOUBLE_EQ(SeekFractionAt(150.f, 100.f, 0.f), 0.0) << "an empty bar";
}

TEST(VoiceBubbleRender, WithoutAPlayerASoundFileIsAFileRowToOpen) {
  const std::string rml = Render(Attachment("m7", "Song.mp3", "audio/mpeg", 10800000), Facts(true));
  EXPECT_EQ(rml.find("play_audio_file"), std::string::npos);
  EXPECT_NE(rml.find("open_attachment('m7')"), std::string::npos);
}

// Kenneth 2026-11-06 ("播放样式很丑"): the card shows the file's own title, artist and cover when its tags name them,
// its length before it plays, and the name only when the tags say nothing.
TEST(VoiceBubbleRender, ASoundFileCardShowsItsOwnTitleArtistCoverAndLength) {
  TranscriptFacts facts = Facts(true);
  facts.audio_files_play = true;
  facts.attachment = [](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.local_path = "/tmp/x.mp3";
    view.audio_title = "Moon River";
    view.audio_artist = "Audrey <Hepburn>";
    view.audio_duration_ms = 220000;
    view.poster_path = "/tmp/x.poster.jpg";
    return view;
  };
  const std::string rml = Render(Attachment("m7", "track01.mp3", "audio/mpeg", 3200000), facts);
  EXPECT_NE(rml.find(">Moon River</p>"), std::string::npos) << rml;
  EXPECT_EQ(rml.find("track01.mp3"), std::string::npos) << "the tag's title replaces the file name";
  EXPECT_NE(rml.find("Audrey &lt;Hepburn&gt; · 3 MB"), std::string::npos) << rml;
  EXPECT_NE(rml.find("class=\"chat-audio-cover-image\" src=\"/tmp/x.poster.jpg\""), std::string::npos) << rml;
  EXPECT_EQ(rml.find("chat-audio-cover-note"), std::string::npos);
  EXPECT_NE(rml.find(">3:40</span>"), std::string::npos) << "its length before it plays";
}

TEST(VoiceBubbleRender, APlayingSoundFileKeepsItsProgressOnARedraw) {
  const VoiceBubbleState state{.playing = true, .paused = false, .position_ms = 65000, .duration_ms = 130000};
  const std::string rml =
      AudioFileBubbleRml("m7", "m7", AudioCardFace{.title = "Song.mp3", .subtitle = "10 MB"}, state);
  EXPECT_NE(rml.find("chat-voice-play--playing"), std::string::npos);
  EXPECT_NE(rml.find("width: 50%"), std::string::npos);
  EXPECT_NE(rml.find("1:05 / 2:10"), std::string::npos);
}

// Kenneth 2026-11-03: a blurry picture in the chat could not be enlarged — tapping it did nothing.
TEST(VoiceBubbleRender, TappingAPictureOpensItFull) {
  const std::string rml = Render(Attachment("p1", "photo.jpg", "image/jpeg", 50000), Facts(true));
  EXPECT_NE(rml.find("chat-attachment-image"), std::string::npos) << rml;
  // B203: the message id only; the full picture is found from the message (the bubble shows only its preview).
  EXPECT_NE(rml.find("data-event-click=\"open_picture('p1')\""), std::string::npos) << rml;
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

// Kenneth 2026-11-04 (PLAN-ui-refresh-impl 62.3): pictures spilled out of their bubble; 2026-11-09: like WhatsApp,
// a picture takes the box width (292dp, about three quarters of the phone) and a tall one is cropped to the box
// height (360dp) showing its middle, instead of shrinking to a sliver.
TEST(PictureBubbleFit, EveryShapeFitsTheBoxWithItsOwnRatio) {
  const auto fit = [](const int w, const int h) {
    const PictureBubbleSize size = FitPictureInBubble(w, h);
    return std::make_pair(size.width_dp, size.height_dp);
  };
  EXPECT_EQ(fit(4032, 3024), std::make_pair(292, 219)) << "a landscape photo";
  EXPECT_EQ(fit(3024, 4032), std::make_pair(292, 360)) << "a portrait photo keeps the width, cropped to the box height";
  EXPECT_EQ(fit(600, 2400), std::make_pair(292, 360)) << "very tall: full width, the middle shows";
  EXPECT_EQ(fit(2400, 400), std::make_pair(292, 49)) << "very wide";
  EXPECT_EQ(fit(20000, 10), std::make_pair(292, 1)) << "a sliver keeps 1dp";
  EXPECT_EQ(fit(300, 200), std::make_pair(292, 195)) << "a mid picture shrinks to the width";
  EXPECT_EQ(fit(160, 130), std::make_pair(160, 130)) << "a picture with a short side over 120 is not enlarged";
  EXPECT_EQ(fit(120, 90), std::make_pair(160, 120)) << "a small picture grows to a 120dp short side";
  EXPECT_EQ(fit(0, 300), std::make_pair(0, 0)) << "unknown size";
  // The cropped picture is drawn at its full fitted height, shifted up by half the excess.
  const PictureBubbleSize tall = FitPictureInBubble(3024, 4032);
  EXPECT_EQ(tall.image_height_dp, 389);
  EXPECT_EQ(tall.image_offset_dp, 14);
  const PictureBubbleSize wide = FitPictureInBubble(4032, 3024);
  EXPECT_EQ(wide.image_height_dp, 219);
  EXPECT_EQ(wide.image_offset_dp, 0);
}

TEST(PictureBubbleFit, ThePictureCarriesItsFittedSize) {
  TranscriptFacts facts;
  facts.attachment = [](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.local_path = "/tmp/tall.jpg";
    view.image_width = 3024; // upright: InboxController has swapped a quarter-turned photo's sides
    view.image_height = 4032;
    return view;
  };
  const std::string rml = Render(Attachment("p3", "tall.jpg", "image/jpeg", 50000), facts);
  EXPECT_NE(rml.find("width=\"292\" height=\"389\" style=\"margin-top:-14dp\""), std::string::npos) << rml;
  // The picture sits in a same-sized clip box that rounds its corners (the engine does not round an <img>).
  EXPECT_NE(rml.find("<div class=\"chat-attachment-clip\" style=\"width:292dp;height:360dp\" pic-id=\"p3\""),
            std::string::npos)
      << rml;
  EXPECT_NE(rml.find("\"><img class=\"chat-attachment-image\""), std::string::npos) << rml;
  EXPECT_NE(rml.find("')\"/></div>"), std::string::npos) << rml;
  // Without a known size the picture is left to the stylesheet's limits.
  const std::string unknown = Render(Attachment("p4", "x.jpg", "image/jpeg", 50000), Facts(true));
  EXPECT_NE(unknown.find("class=\"chat-attachment-image\" src=\"/tmp/x\" data-event-click="), std::string::npos)
      << unknown;
}

// Kenneth 2026-11-06 (iPhone): a video was a black box wider than its bubble with an Open button. Now the poster sits
// in a clipped frame of explicit dp size with a play disc, and a tap on the frame opens the system player.
TEST(VideoBubbleRender, APosterFrameWithPlayDiscAndNoOpenButton) {
  TranscriptFacts facts = Facts(true);
  facts.attachment = [](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.local_path = "/tmp/v.mp4";
    view.poster_path = "/tmp/v.poster.jpg";
    view.image_width = 1280;
    view.image_height = 720;
    return view;
  };
  const std::string rml = Render(Attachment("v1", "v.mp4", "video/mp4", 289000), facts);
  EXPECT_NE(rml.find("chat-attachment-clip chat-attachment-video-frame\" style=\"width:292dp;height:164dp\""),
            std::string::npos)
      << rml;
  EXPECT_NE(rml.find("width=\"292\" height=\"164\""), std::string::npos) << rml;
  EXPECT_NE(rml.find("chat-attachment-video-play"), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"open_attachment('v1')\""), std::string::npos);
  EXPECT_EQ(rml.find("chat-suggestion"), std::string::npos) << "no Open button";
  EXPECT_NE(rml.find("282 KB"), std::string::npos);
  // Unknown poster size: the 216 x 122dp fallback.
  facts.attachment = [](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.local_path = "/tmp/v.mp4";
    view.poster_path = "/tmp/v.poster.jpg";
    return view;
  };
  EXPECT_NE(Render(Attachment("v1", "v.mp4", "video/mp4", 289000), facts).find("width:216dp;height:122dp"),
            std::string::npos);
}

TEST(VideoBubbleRender, WithoutAPosterTheBadgeLayoutIsStillTappable) {
  TranscriptFacts facts = Facts(true);
  facts.attachment = [](const ThreadMessage&) {
    TranscriptAttachmentView view;
    view.state = AttachmentFetchWorkflow::DownloadState::Ready;
    view.local_path = "/tmp/v.mp4";
    return view;
  };
  const std::string rml = Render(Attachment("v2", "v.mp4", "video/mp4", 289000), facts);
  EXPECT_NE(rml.find("chat-attachment-video-badge"), std::string::npos) << rml;
  EXPECT_NE(rml.find("chat-attachment-video-play"), std::string::npos);
  EXPECT_NE(rml.find("data-event-click=\"open_attachment('v2')\""), std::string::npos);
  EXPECT_EQ(rml.find("chat-suggestion"), std::string::npos);
}

} // namespace
} // namespace pbr
