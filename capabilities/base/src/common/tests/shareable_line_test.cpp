#include "common/Diag.h"
#include "common/Metrics.h"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <string_view>
#include <utility>

namespace pbr {
namespace {

// What leaves the device cannot be free text: no runtime string goes in as a value or a key.
template <class T>
concept TakesAsValue = requires(ShareableLine& line, T value) { line.Add("key", value); };
template <class T>
concept TakesAsKey = requires(ShareableLine& line, T key) { line.Add(key, 1); };
static_assert(!TakesAsValue<std::string>);
static_assert(!TakesAsValue<std::string_view>);
static_assert(!TakesAsKey<std::string>);
static_assert(TakesAsValue<int> && TakesAsValue<bool> && TakesAsValue<ShareWord>);

TEST(ShareableLineTest, WritesEventThenKeyValuePairs) {
  const std::string text = DiagLine("relay.http_failed")
                               .Add("action", "send")
                               .Add("status", 503)
                               .Add("big", uint64_t{18446744073709551615u})
                               .Add("ok", false)
                               .Add("rate", 2.25)
                               .Add("waited", std::chrono::seconds(2))
                               .str();
  EXPECT_EQ(text, "event=relay.http_failed action=send status=503 big=18446744073709551615 ok=0 rate=2.2 waited=2000");
}

constexpr ShareWord kRoutes[] = {"server", "local"};

TEST(ShareableLineTest, PickKeepsOnlyTheListedWords) {
  EXPECT_EQ(MetricsLine("ai.turn").Pick("route", "local", kRoutes).str(), "event=ai.turn route=local");
  EXPECT_EQ(MetricsLine("ai.turn").Pick("route", "Alice's phone", kRoutes).str(),
            "event=ai.turn route=other");
  EXPECT_EQ(MetricsLine("ai.turn").Pick("route", "", kRoutes).str(), "event=ai.turn route=-");
}

TEST(ShareableLineTest, RefJoinsOneRunWithoutShowingTheId) {
  const std::string id = "account:Q2xhcmEgaXMgbm90IGEgcmVhbCBhY2NvdW50IGlkIGF0IGFsbA";
  const std::string ref = ShareableRef(id);
  ASSERT_EQ(ref.size(), 8u);
  EXPECT_EQ(ref.find_first_not_of("0123456789abcdef"), std::string::npos);
  EXPECT_EQ(ShareableRef(id), ref) << "the same id gives the same ref within a run";
  EXPECT_EQ(ShareableRef(id.substr(8)), ref) << "with or without its scheme";
  EXPECT_NE(ShareableRef("account:someone-else"), ref);
  const std::string text = DiagLine("x").Ref("peer", id).str();
  EXPECT_EQ(text, "event=x peer=" + ref);
  EXPECT_EQ(text.find("Q2xh"), std::string::npos);
  EXPECT_EQ(DiagLine("x").Ref("peer", "").str(), "event=x peer=-");
}

TEST(ShareableLineTest, CallKeepsEightHexOfARandomCallIdOnly) {
  EXPECT_EQ(DiagLine("x").Call("call", "call:0123456789abcdef0123456789abcdef").str(), "event=x call=01234567");
  EXPECT_EQ(DiagLine("x").Call("call", "alice@example.com").str(), "event=x call=?");
  EXPECT_EQ(MetricsCallId("call:89abcdef01234567"), "89abcdef");
}

TEST(ShareableLineTest, FailWritesCategoryAndCodeNeverTheMessage) {
  const Error error = Error::Make(3, 17, "relay said: alice@example.com is unknown");
  const std::string text = DiagLine("x").Fail("error", error).str();
  EXPECT_EQ(text, "event=x error=3.17");
}

TEST(ShareableLineTest, EmitWritesOnItsOwnChannel) {
  struct Capture : logging::Handler {
    void emit(logging::Level, const std::string& name, const std::string& message) override {
      lines.emplace_back(name, message);
    }
    std::vector<std::pair<std::string, std::string>> lines;
  };
  auto capture = std::make_shared<Capture>();
  DiagLog().addHandler(capture);
  DiagLine("unlock.done").Add("unlocked", true).Emit();
  ASSERT_FALSE(capture->lines.empty());
  EXPECT_EQ(capture->lines.back().first, "Diag");
  EXPECT_NE(capture->lines.back().second.find("event=unlock.done unlocked=1"), std::string::npos);
}

} // namespace
} // namespace pbr
