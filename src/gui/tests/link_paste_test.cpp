// What the two "link a new device" pages accept when the person pastes the other device's code (a chat, an email, a
// note): the text form of an offer or a reply, found anywhere in the pasted block. FindLinkPayload belongs to the
// handshake's owner and is only called here; a chat-mangled form it rejects is a DISABLED case below, with where it
// fails, to be raised with that owner (not fixed in this branch).
#include "foundation/crypto/LinkHandshake.h"
#include "foundation/i18n/LocalizationService.h"
#include "gui/LinkDeviceText.h"

#include <gtest/gtest.h>

#include <string>

namespace pbr {
namespace {

// A payload as long as a real reply (about 1.4 KB, first byte below kLinkPayloadVersionLimit); FindLinkPayload only
// decodes it, the sealed content is not read here.
ByteVector Payload() {
  ByteVector bytes(1400);
  uint32_t x = 12345;
  for (uint8_t& b : bytes) {
    x = x * 1664525U + 1013904223U;
    b = static_cast<uint8_t>(x >> 24);
  }
  bytes[0] = 0x01;
  return bytes;
}

// Chat apps that wrap long words: a line break every `width` characters.
std::string Wrapped(const std::string& text, const size_t width, const std::string& newline) {
  std::string out;
  for (size_t i = 0; i < text.size(); i += width) {
    out += (i == 0 ? "" : newline) + text.substr(i, width);
  }
  return out;
}

class LinkPasteTest : public testing::TestWithParam<const char*> {
protected:
  const std::string prefix = GetParam();
  const ByteVector payload = Payload();
  const std::string text = LinkPayloadText(prefix, payload);
  bool Accepts(const std::string& pasted) const {
    const auto found = FindLinkPayload(prefix, pasted);
    return found && *found == payload;
  }
};

TEST_P(LinkPasteTest, TheCodeAloneIsAccepted) {
  EXPECT_TRUE(Accepts(text));
}

TEST_P(LinkPasteTest, TheCodeBetweenChatTextIsAccepted) {
  EXPECT_TRUE(Accepts("Here is the code for my new phone:\n" + text + "\nthanks!"));
  EXPECT_TRUE(Accepts("Here you go " + text + " (it is valid for 15 minutes)."));
  EXPECT_TRUE(Accepts("好的，码在这里：" + text + "，麻烦看一下"));
}

TEST_P(LinkPasteTest, LeadingAndTrailingSpacesAndBlankLinesAreAccepted) {
  EXPECT_TRUE(Accepts("   " + text + "   "));
  EXPECT_TRUE(Accepts("\n\n\t " + text + " \t\n\n"));
  EXPECT_TRUE(Accepts("\r\n" + text + "\r\n"));
}

TEST_P(LinkPasteTest, AQuotedReplyPrefixIsAccepted) {
  EXPECT_TRUE(Accepts("> " + text));
  EXPECT_TRUE(Accepts("> Kenneth wrote:\n> " + text + "\n\nGot it"));
  EXPECT_TRUE(Accepts(">> " + text));
}

TEST_P(LinkPasteTest, ADuplicatedCodeIsAccepted) {
  EXPECT_TRUE(Accepts(text + "\n" + text));
  EXPECT_TRUE(Accepts(text + " " + text));
  EXPECT_TRUE(Accepts("> " + text + "\n\n" + text));
}

TEST_P(LinkPasteTest, APasteWithoutACodeIsRefusedWithThePlainWords) {
  for (const char* pasted : {"", "   ", "hello, here is nothing", "pp-browser-contact-v1:eyJhY2NvdW50X2lkIjoiYSJ9",
                             "https://example.org/some/long/path?x=1"}) {
    EXPECT_FALSE(FindLinkPayload(prefix, pasted)) << pasted;
  }
  // The same-named other kind of code (an offer pasted where a reply is wanted) is not this one either.
  const std::string other = std::string(prefix) == kLinkOfferTextPrefix ? kLinkReplyTextPrefix : kLinkOfferTextPrefix;
  EXPECT_FALSE(FindLinkPayload(prefix, LinkPayloadText(other, payload)));
  // What the callers (ConversationsHub::LinkOfferCode / SealLinkReply / ImportLinkReply) then return, and its words.
  const Error error = MakeLinkError(LinkError::Unreadable, "link: no code in what was given");
  ASSERT_TRUE(LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  for (const char* lang : {"en", "zh-Hans"}) {
    LocalizationService::Instance().SetPreferredLanguage(lang);
    EXPECT_FALSE(LinkErrorText(error, /*on_new_device=*/true).empty()) << lang;
    EXPECT_FALSE(LinkErrorText(error, /*on_new_device=*/false).empty()) << lang;
  }
}

// DISABLED: a chat app that wraps the long code every N characters (a newline, CRLF or a space inserted inside the
// base64url run). FindLinkPayload (capabilities/base/src/foundation/crypto/LinkHandshake.cpp, the loop that collects
// characters after the prefix) stops at the first character that is not base64url, so it takes only the first line;
// the rest is dropped, the decoded bytes are the truncated head and are not the payload. The caller then fails on the
// truncated payload (ReadLinkOffer / link_offer_.Open) instead of finding "no code". To raise with the handshake's
// owner; not fixed here.
TEST_P(LinkPasteTest, DISABLED_ACodeWrappedByAChatAppIsAccepted) {
  EXPECT_TRUE(Accepts(Wrapped(text, 76, "\n"))) << "newline every 76 characters";
  EXPECT_TRUE(Accepts(Wrapped(text, 40, "\r\n"))) << "CRLF every 40 characters";
  EXPECT_TRUE(Accepts(Wrapped(text, 60, " "))) << "a space every 60 characters";
  EXPECT_TRUE(Accepts("> " + Wrapped(text, 70, "\n> "))) << "wrapped and quoted line by line";
}

// What does happen today with a wrapped code, so the report is exact: the first line is read as if it were the code.
TEST_P(LinkPasteTest, AWrappedCodeIsNotTakenForTheWholeCode) {
  const auto found = FindLinkPayload(prefix, Wrapped(text, 76, "\n"));
  EXPECT_FALSE(found && *found == payload);
}

INSTANTIATE_TEST_SUITE_P(BothCodes, LinkPasteTest, testing::Values(kLinkOfferTextPrefix, kLinkReplyTextPrefix));

} // namespace
} // namespace pbr
