// DEV/PLAN-desktop-drop-paste.md, D1: what the clipboard holds by type, and the PNG read from it. Every test runs on a
// fake clipboard (the ClipboardSource seam): the real system clipboard is never touched.
#include "foundation/platform/ClipboardAttachments.h"

#include <gtest/gtest.h>

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

  ClipboardSource Source() {
    ClipboardSource source;
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
