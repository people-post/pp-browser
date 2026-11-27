// DEV/PLAN-desktop-drop-paste.md, D1: what the clipboard holds by type, and the PNG read from it. Every test runs on a
// fake clipboard (the ClipboardSource seam): the real system clipboard is never touched.
#include "foundation/platform/ClipboardAttachments.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

namespace pbr {
namespace {

/** A fake clipboard that records what was asked of it. */
struct FakeClipboard {
  std::vector<std::string> types;
  std::string text;
  std::map<std::string, std::string> data;
  int type_calls = 0;
  int text_calls = 0;
  int data_calls = 0;
  /** The fresh query (SDL_HasClipboardData): which types it says are there, and whether it is cheap on this platform. */
  bool with_fresh_query = false;
  bool query_is_cheap = true;
  std::vector<std::string> fresh_types;
  int has_data_calls = 0;

  ClipboardSource Source() {
    ClipboardSource source;
    if (with_fresh_query) {
      source.has_data = [this](const std::string& mime) {
        ++has_data_calls;
        return std::find(fresh_types.begin(), fresh_types.end(), mime) != fresh_types.end();
      };
      source.fresh_query_cheap = [this]() { return query_is_cheap; };
    }
    source.mime_types = [this]() {
      ++type_calls;
      return types;
    };
    source.text = [this]() {
      ++text_calls;
      return text;
    };
    source.data = [this](const std::string& mime) -> std::optional<std::string> {
      ++data_calls;
      auto it = data.find(mime);
      if (it == data.end()) {
        return std::nullopt;
      }
      return it->second;
    };
    return source;
  }
};

const std::string kPngHead = std::string("\x89PNG\r\n\x1a\n", 8);
const std::string kJpegHead = std::string("\xFF\xD8\xFF\xE0", 4);

// A screenshot tool wrote the clipboard after SDL's cached type list was taken (macOS and Windows refresh it only when
// the window gains focus): the list still shows the old content, the fresh query shows the picture.
TEST(ClipboardPeekTest, AStaleTypeListWithAFreshPngIsAReadablePicture) {
  FakeClipboard clipboard;
  clipboard.with_fresh_query = true;
  clipboard.types = {}; // the cache: nothing, or only what was copied before
  clipboard.fresh_types = {"image/png"};
  ClipboardSummary summary = PeekClipboard(clipboard.Source());
  EXPECT_EQ(summary.image_type, "image/png");
  EXPECT_FALSE(summary.has_unusable_image);
  EXPECT_FALSE(summary.has_text);
  EXPECT_EQ(clipboard.data_calls, 0) << "types only: no data is fetched";
  // The old copy was text: the stale list still says text, but a picture is there now, so the text must be non-blank to win
  // (the real text is read afresh: the screenshot has none).
  clipboard.types = {"text/plain;charset=utf-8"};
  clipboard.text = "";
  summary = PeekClipboard(clipboard.Source());
  EXPECT_EQ(summary.image_type, "image/png");
  EXPECT_FALSE(summary.has_text) << "the text of the old copy is gone: the picture is pasted";
}

TEST(ClipboardPeekTest, WhereTheFreshQueryIsNotCheapOnlyTheTypeListIsUsedAndHasDataIsNeverCalled) {
  FakeClipboard clipboard;
  clipboard.with_fresh_query = true;
  clipboard.query_is_cheap = false;
  clipboard.fresh_types = {"image/png", "image/jpeg", "image/tiff"};
  clipboard.types = {};
  ClipboardSummary summary = PeekClipboard(clipboard.Source());
  EXPECT_TRUE(summary.image_type.empty());
  EXPECT_FALSE(summary.has_unusable_image);
  clipboard.types = {"image/png"};
  summary = PeekClipboard(clipboard.Source());
  EXPECT_EQ(summary.image_type, "image/png") << "the list is read as before";
  EXPECT_EQ(clipboard.has_data_calls, 0) << "SDL_HasClipboardData would transfer the picture on X11";
}

TEST(ClipboardPeekTest, ATiffOnlyOrBmpOnlyFreshAnswerIsAnUnusableImage) {
  for (const char* type : {"image/tiff", "image/bmp"}) {
    FakeClipboard clipboard;
    clipboard.with_fresh_query = true;
    clipboard.types = {};
    clipboard.fresh_types = {type};
    const ClipboardSummary summary = PeekClipboard(clipboard.Source());
    EXPECT_TRUE(summary.image_type.empty()) << type;
    EXPECT_TRUE(summary.has_unusable_image) << type << ": the pill, not silence";
  }
}

TEST(ClipboardPeekTest, AJpegIsReadableAndAPngIsPreferredToIt) {
  FakeClipboard clipboard;
  clipboard.types = {"image/jpeg"};
  EXPECT_EQ(PeekClipboard(clipboard.Source()).image_type, "image/jpeg");
  EXPECT_FALSE(PeekClipboard(clipboard.Source()).has_unusable_image);
  clipboard.types = {};
  clipboard.with_fresh_query = true;
  clipboard.fresh_types = {"image/jpeg", "image/png"};
  EXPECT_EQ(PeekClipboard(clipboard.Source()).image_type, "image/png");
  clipboard.fresh_types = {"image/jpeg"};
  EXPECT_EQ(PeekClipboard(clipboard.Source()).image_type, "image/jpeg");
}

TEST(ClipboardReadTest, AJpegIsReadWithItsSignatureAndTheExtensionJpg) {
  FakeClipboard clipboard;
  clipboard.data["image/jpeg"] = kJpegHead + "body";
  const auto image = ReadClipboardImage(clipboard.Source(), 1000);
  ASSERT_TRUE(image.has_value());
  EXPECT_EQ(image->extension, "jpg");
  EXPECT_EQ(image->bytes, kJpegHead + "body");
  clipboard.data["image/jpeg"] = "not a jpeg";
  EXPECT_FALSE(ReadClipboardImage(clipboard.Source(), 1000).has_value()) << "the signature is checked like the PNG's";
  clipboard.data["image/png"] = kPngHead + "x";
  const auto png = ReadClipboardImage(clipboard.Source(), 1000);
  ASSERT_TRUE(png.has_value());
  EXPECT_EQ(png->extension, "png") << "a PNG wins when both are there";
}

TEST(ClipboardPeekTest, PngOnlyIsAReadablePictureWithNoTextAndNoFiles) {
  FakeClipboard clipboard;
  clipboard.types = {"image/png"};
  const ClipboardSummary summary = PeekClipboard(clipboard.Source());
  EXPECT_EQ(summary.image_type, "image/png");
  EXPECT_FALSE(summary.has_text);
  EXPECT_FALSE(summary.has_files);
  EXPECT_FALSE(summary.has_unusable_image);
  EXPECT_EQ(clipboard.data_calls, 0) << "types only: no data is fetched";
  EXPECT_EQ(clipboard.text_calls, 0);
}

TEST(ClipboardPeekTest, PngWithTextIsTextWhenTheTextIsNotBlank) {
  FakeClipboard clipboard;
  clipboard.types = {"image/png", "text/plain;charset=utf-8"};
  clipboard.text = "a cell";
  const ClipboardSummary summary = PeekClipboard(clipboard.Source());
  EXPECT_TRUE(summary.has_text);
  EXPECT_EQ(summary.image_type, "image/png");
  EXPECT_EQ(clipboard.text_calls, 1);
}

TEST(ClipboardPeekTest, PngWithBlankTextIsNotText) {
  FakeClipboard clipboard;
  clipboard.types = {"image/png", "text/plain"};
  clipboard.text = " \n\t ";
  EXPECT_FALSE(PeekClipboard(clipboard.Source()).has_text);
  clipboard.text = "";
  EXPECT_FALSE(PeekClipboard(clipboard.Source()).has_text);
}

TEST(ClipboardPeekTest, TextIsOnlyReadWhenAPictureTypeIsThereToo) {
  FakeClipboard clipboard;
  clipboard.types = {"text/plain"};
  clipboard.text = "hello";
  const ClipboardSummary summary = PeekClipboard(clipboard.Source());
  EXPECT_TRUE(summary.has_text);
  EXPECT_EQ(clipboard.text_calls, 0) << "nothing to decide against: the text is not read";
}

TEST(ClipboardPeekTest, BmpOnlyIsAnUnusablePicture) {
  FakeClipboard clipboard;
  clipboard.types = {"image/bmp"};
  const ClipboardSummary summary = PeekClipboard(clipboard.Source());
  EXPECT_TRUE(summary.image_type.empty());
  EXPECT_TRUE(summary.has_unusable_image);
}

TEST(ClipboardPeekTest, TiffAndPngTogetherAreReadableNotUnusable) {
  FakeClipboard clipboard;
  clipboard.types = {"image/tiff", "image/png"};
  const ClipboardSummary summary = PeekClipboard(clipboard.Source());
  EXPECT_EQ(summary.image_type, "image/png");
  EXPECT_FALSE(summary.has_unusable_image);
}

TEST(ClipboardPeekTest, AFileAddressTypeIsFilesAndANameAsTextBesideIt) {
  FakeClipboard clipboard;
  clipboard.types = {"public.file-url", "text/plain;charset=utf-8"};
  clipboard.text = "photo.png";
  const ClipboardSummary summary = PeekClipboard(clipboard.Source());
  EXPECT_TRUE(summary.has_files);
  EXPECT_TRUE(summary.has_text);

  FakeClipboard linux_clipboard;
  linux_clipboard.types = {"text/uri-list"};
  EXPECT_TRUE(PeekClipboard(linux_clipboard.Source()).has_files);
}

TEST(ClipboardPeekTest, NothingOnTheClipboardIsEmpty) {
  FakeClipboard clipboard;
  const ClipboardSummary summary = PeekClipboard(clipboard.Source());
  EXPECT_FALSE(summary.has_files);
  EXPECT_FALSE(summary.has_text);
  EXPECT_FALSE(summary.has_unusable_image);
  EXPECT_TRUE(summary.image_type.empty());
}

TEST(ClipboardPeekTest, AnEmptySourceIsEmptyToo) {
  EXPECT_TRUE(PeekClipboard(ClipboardSource{}).image_type.empty());
}

// S2: asked only when copied files read back empty and the text paste might be the right one after all.
TEST(ClipboardHasTextTest, ATextTypeWithNonBlankTextIsText) {
  FakeClipboard clipboard;
  clipboard.types = {"public.file-url", "text/plain;charset=utf-8"};
  clipboard.text = "photo.png";
  EXPECT_TRUE(ClipboardHasText(clipboard.Source()));
}

TEST(ClipboardHasTextTest, NoTextTypeBlankTextAndNoSourceAreNotText) {
  FakeClipboard clipboard;
  clipboard.types = {"text/uri-list"};
  clipboard.text = "left over";
  EXPECT_FALSE(ClipboardHasText(clipboard.Source())) << "no text type: the text is not even read";
  EXPECT_EQ(clipboard.text_calls, 0);
  clipboard.types = {"text/plain"};
  clipboard.text = " \n ";
  EXPECT_FALSE(ClipboardHasText(clipboard.Source()));
  EXPECT_FALSE(ClipboardHasText(ClipboardSource{}));
}

TEST(ClipboardHasTextTest, PeekDoesNotReadTheTextWhenOnlyFilesAreThere) {
  FakeClipboard clipboard;
  clipboard.types = {"public.file-url", "text/plain"};
  clipboard.text = "photo.png";
  PeekClipboard(clipboard.Source());
  EXPECT_EQ(clipboard.text_calls, 0) << "the text is read lazily, at the empty-read fallback only";
}

TEST(ClipboardReadTest, APngComesBackWithItsExtension) {
  FakeClipboard clipboard;
  clipboard.data["image/png"] = kPngHead + "rest";
  const auto image = ReadClipboardImage(clipboard.Source(), 1000);
  ASSERT_TRUE(image.has_value());
  EXPECT_FALSE(image->too_large);
  EXPECT_EQ(image->extension, "png");
  EXPECT_EQ(image->bytes, kPngHead + "rest");
}

TEST(ClipboardReadTest, MoreThanTheLimitIsTooLargeAndCarriesNoBytes) {
  FakeClipboard clipboard;
  clipboard.data["image/png"] = kPngHead + std::string(100, 'x');
  const auto image = ReadClipboardImage(clipboard.Source(), 50);
  ASSERT_TRUE(image.has_value());
  EXPECT_TRUE(image->too_large);
  EXPECT_TRUE(image->bytes.empty());
}

TEST(ClipboardReadTest, ExactlyTheLimitIsAccepted) {
  FakeClipboard clipboard;
  clipboard.data["image/png"] = kPngHead + "12";
  const auto image = ReadClipboardImage(clipboard.Source(), 10);
  ASSERT_TRUE(image.has_value());
  EXPECT_FALSE(image->too_large);
}

TEST(ClipboardReadTest, BytesThatAreNotAPngAreUnusable) {
  FakeClipboard clipboard;
  clipboard.data["image/png"] = "GIF89a....";
  EXPECT_FALSE(ReadClipboardImage(clipboard.Source(), 1000).has_value());
  clipboard.data["image/png"] = "\x89PN";
  EXPECT_FALSE(ReadClipboardImage(clipboard.Source(), 1000).has_value()) << "shorter than the signature";
}

TEST(ClipboardReadTest, NoDataEmptyDataAndNoSourceAreNothing) {
  FakeClipboard clipboard;
  EXPECT_FALSE(ReadClipboardImage(clipboard.Source(), 1000).has_value());
  clipboard.data["image/png"] = "";
  EXPECT_FALSE(ReadClipboardImage(clipboard.Source(), 1000).has_value());
  EXPECT_FALSE(ReadClipboardImage(ClipboardSource{}, 1000).has_value());
}

TEST(ClipboardReadTest, TheNoArgumentCallsUseTheSourceTestsInstall) {
  FakeClipboard clipboard;
  clipboard.types = {"image/png"};
  clipboard.data["image/png"] = kPngHead;
  SetClipboardSourceForTest(clipboard.Source());
  EXPECT_EQ(PeekClipboard().image_type, "image/png");
  EXPECT_TRUE(ReadClipboardImage(1000).has_value());
  SetClipboardSourceForTest(std::nullopt);
}

// Not run by default: prints which TYPES the real clipboard holds right now, so the owner can check what each OS's
// screenshot tool and "copy image" put there (DEV/PLAN-desktop-drop-paste.md, step 0). Run it by hand, with consent:
//   pp_browser_platform_test --gtest_also_run_disabled_tests --gtest_filter='*DISABLED_ClipboardTypesProbe'
// It prints type NAMES and, for text types, the text LENGTH. Never a text's content, never any picture bytes.
TEST(ClipboardProbeTest, DISABLED_ClipboardTypesProbe) {
  const ClipboardSource source = SdlClipboardSource();
  const std::vector<std::string> types = source.mime_types();
  std::printf("clipboard types: %zu\n", types.size());
  for (const std::string& type : types) {
    const bool text = type.rfind("text/", 0) == 0 || type == "UTF8_STRING" || type == "STRING" || type == "TEXT" ||
                      type.find("plain-text") != std::string::npos;
    if (text && type != "text/uri-list") {
      const std::optional<std::string> data = source.data(type);
      std::printf("  %s (text, %zu bytes)\n", type.c_str(), data ? data->size() : size_t{0});
    } else {
      std::printf("  %s\n", type.c_str());
    }
  }
  SUCCEED();
}

} // namespace
} // namespace pbr
