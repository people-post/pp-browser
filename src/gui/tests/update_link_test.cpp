// Which update link the "new version" card offers, per platform (UsableUpdateLink).
#include "gui/UpdateLink.h"

#include <gtest/gtest.h>

namespace pbr {
namespace {

const std::string kFallback = "https://github.com/people-post/pp-browser/releases";

ClientCompatDocument Doc(const std::string& generic = {}, const std::map<std::string, std::string>& per_platform = {}) {
  ClientCompatDocument doc;
  doc.schema_version = 1;
  doc.upgrade_url = generic;
  doc.upgrade_urls = per_platform;
  return doc;
}

TEST(UpdateLinkTest, APlaceholderOrNonHttpsLinkCountsAsNoLink) {
  for (const char* bad :
       {"https://example.test/releases", "https://dl.example.test/x", "http://apps.apple.com/app/id1",
        "https://example.com/x", "https://www.example.org/", "https://a.invalid/", "https://localhost/",
        "https:///path", "https://", "ftp://x.com/y", "https://x.com/a b", "", "apps.apple.com"}) {
    EXPECT_FALSE(IsUsableUpdateUrl(bad)) << bad;
  }
  for (const char* good : {"https://apps.apple.com/app/id123", "https://testflight.apple.com/join/abc",
                           "https://notexample.com/x", "https://pp.example.dev/x", "https://xn--mnchen-3ya.de/x",
                           "https://APPS.Apple.com/app/id1", "https://1password.com/x"}) {
    EXPECT_TRUE(IsUsableUpdateUrl(good)) << good;
  }
}

TEST(UpdateLinkTest, APlaceholderGivesNoButtonOnPhonesAndLinuxAndTheReleasesPageOnTheDesktopOnes) {
  const ClientCompatDocument doc = Doc("https://example.test/releases");
  for (const char* platform : {"ios", "android", "linux"}) {
    EXPECT_EQ(UsableUpdateLink(doc, platform, kFallback), "") << platform;
  }
  for (const char* platform : {"macos", "windows"}) {
    EXPECT_EQ(UsableUpdateLink(doc, platform, kFallback), kFallback) << platform;
  }
  const ClientCompatDocument sub = Doc("https://dl.example.test/x");
  EXPECT_EQ(UsableUpdateLink(sub, "ios", kFallback), "");
  EXPECT_EQ(UsableUpdateLink(Doc("http://insecure.com/x"), "android", kFallback), "");
}

TEST(UpdateLinkTest, AnotherSpellingOfTheReleasesPageIsTheFallbackToo) {
  for (const char* variant :
       {"HTTPS://GitHub.com/people-post/pp-browser/releases", "https://github.com/people-post/pp-browser/releases/",
        "https://GITHUB.COM/people-post/pp-browser/releases//"}) {
    const ClientCompatDocument doc = Doc(variant);
    for (const char* platform : {"ios", "android", "linux"}) {
      EXPECT_EQ(UsableUpdateLink(doc, platform, kFallback), "") << variant << " on " << platform;
    }
    EXPECT_EQ(UsableUpdateLink(doc, "macos", kFallback), variant);
  }
  EXPECT_EQ(UsableUpdateLink(Doc("https://github.com/people-post/pp-browser/releases/tag/v1"), "ios", kFallback),
            "https://github.com/people-post/pp-browser/releases/tag/v1")
      << "a different page on the same site is not the fallback";
}

TEST(UpdateLinkTest, ARealStoreLinkIsOfferedOnItsPlatform) {
  const ClientCompatDocument doc = Doc("", {{"ios", "https://apps.apple.com/app/id123"}});
  EXPECT_EQ(UsableUpdateLink(doc, "ios", kFallback), "https://apps.apple.com/app/id123");
  EXPECT_EQ(UsableUpdateLink(doc, "android", kFallback), "") << "no link for another platform, no fallback on Android";
  EXPECT_EQ(UsableUpdateLink(doc, "macos", kFallback), kFallback);
  EXPECT_EQ(UsableUpdateLink(Doc("", {{"ios", "https://testflight.apple.com/join/abc"}}), "ios", kFallback),
            "https://testflight.apple.com/join/abc");
}

TEST(UpdateLinkTest, NoLinkAtAllIsTheFallbackOnMacAndWindowsOnly) {
  const ClientCompatDocument doc = Doc();
  EXPECT_EQ(UsableUpdateLink(doc, "macos", kFallback), kFallback);
  EXPECT_EQ(UsableUpdateLink(doc, "windows", kFallback), kFallback);
  for (const char* platform : {"ios", "android", "linux"}) {
    EXPECT_EQ(UsableUpdateLink(doc, platform, kFallback), "") << platform;
  }
}

TEST(UpdateLinkTest, TheDocumentsOwnLinkEqualToTheFallbackFollowsTheSameRule) {
  const ClientCompatDocument doc = Doc(kFallback);
  EXPECT_EQ(UsableUpdateLink(doc, "macos", kFallback), kFallback);
  EXPECT_EQ(UsableUpdateLink(doc, "windows", kFallback), kFallback);
  for (const char* platform : {"ios", "android", "linux"}) {
    EXPECT_EQ(UsableUpdateLink(doc, platform, kFallback), "")
        << platform << ": the releases page is not where it installs from";
  }
}

TEST(UpdateLinkTest, APlaceholderForThisPlatformLetsARealGenericLinkWin) {
  const ClientCompatDocument doc = Doc("https://downloads.pp.global/get", {{"ios", "https://example.test/releases"}});
  EXPECT_EQ(UsableUpdateLink(doc, "ios", kFallback), "https://downloads.pp.global/get");
}

} // namespace
} // namespace pbr
