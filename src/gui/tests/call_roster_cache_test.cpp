// DEV/PLAN-call-frame-cost.md step 1: the in-call roster no longer reads every member's name and avatar from disk on
// every frame. Names and avatar paths — found or not (no photo file yet, the "Stranger" placeholder) — are kept for
// the call and read again every 5 s; the call state fields are the screen's, every frame.
#include "gui/CallRosterCache.h"

#include <gtest/gtest.h>

#include <map>
#include <string>
#include <vector>

namespace pbr {
namespace {

constexpr int64_t kT0 = 1'000'000;

struct FakeDirectory {
  std::map<std::string, std::string> names;
  std::map<std::string, std::string> photos;
  std::map<std::string, int> name_reads;
  std::map<std::string, int> photo_reads;

  CallRosterCache::Lookups Lookups() {
    return {.name = [this](const std::string& id) {
              ++name_reads[id];
              auto it = names.find(id);
              return MakeCallerName(it == names.end() ? std::string() : it->second);
            },
            .icon = [this](const std::string& id) {
              ++photo_reads[id];
              auto it = photos.find(id);
              return it == photos.end() ? std::string() : it->second;
            }};
  }
  int PhotoReads() const {
    int n = 0;
    for (const auto& [id, count] : photo_reads) {
      n += count;
    }
    return n;
  }
  int NameReads() const {
    int n = 0;
    for (const auto& [id, count] : name_reads) {
      n += count;
    }
    return n;
  }
};

CallScreen::Member Member(const std::string& id, const bool local = false) {
  CallScreen::Member m;
  m.identity = id;
  m.is_local = local;
  return m;
}

struct Fixture {
  FakeDirectory dir;
  CallRosterCache cache;
  std::vector<CallRosterParticipantState> roster;

  Fixture() {
    dir.names = {{"alice", "Alice"}, {"bob", "Bob"}, {"carol", "Carol"}};
    dir.photos = {{"me", "/p/me.jpg"}, {"alice", "/p/alice.jpg"}, {"bob", "/p/bob.jpg"}, {"carol", "/p/carol.jpg"}};
  }
  void Fill(const std::vector<CallScreen::Member>& members, const int64_t now_ms) {
    cache.ForCall("call-1");
    FillCallRoster(roster, members, "You", cache, now_ms, dir.Lookups());
  }
};

TEST(CallRosterCacheTest, TheSameMembersAreNotReadAgainOnTheNextFrame) {
  Fixture f;
  const std::vector<CallScreen::Member> members = {Member("me", true), Member("alice"), Member("bob")};
  f.Fill(members, kT0);
  EXPECT_EQ(f.dir.PhotoReads(), 3);
  EXPECT_EQ(f.dir.NameReads(), 2) << "our own name is the word for you, never looked up";
  f.dir.photo_reads.clear();
  f.dir.name_reads.clear();
  f.Fill(members, kT0 + 16);
  EXPECT_EQ(f.dir.PhotoReads(), 0);
  EXPECT_EQ(f.dir.NameReads(), 0);
  ASSERT_EQ(f.roster.size(), 3u);
  EXPECT_EQ(f.roster[1].name, "Alice");
  EXPECT_TRUE(f.roster[1].has_avatar);
  EXPECT_EQ(f.roster[1].avatar_src, "/p/alice.jpg");
  EXPECT_EQ(f.roster[0].name, "You");
  EXPECT_EQ(f.roster[0].avatar_src, "/p/me.jpg");
}

TEST(CallRosterCacheTest, ANewMemberIsTheOnlyOneRead) {
  Fixture f;
  f.Fill({Member("me", true), Member("alice")}, kT0);
  f.dir.photo_reads.clear();
  f.dir.name_reads.clear();
  f.Fill({Member("me", true), Member("alice"), Member("carol")}, kT0 + 100);
  EXPECT_EQ(f.dir.photo_reads, (std::map<std::string, int>{{"carol", 1}}));
  EXPECT_EQ(f.dir.name_reads, (std::map<std::string, int>{{"carol", 1}}));
  ASSERT_EQ(f.roster.size(), 3u);
  EXPECT_EQ(f.roster[2].name, "Carol");
}

TEST(CallRosterCacheTest, EveryoneIsReadAgainAfterFiveSecondsNotBefore) {
  Fixture f;
  const std::vector<CallScreen::Member> members = {Member("me", true), Member("alice")};
  f.Fill(members, kT0);
  f.dir.photo_reads.clear();
  f.dir.name_reads.clear();
  f.Fill(members, kT0 + 4900);
  EXPECT_EQ(f.dir.PhotoReads(), 0) << "4.9 s";
  f.dir.names["alice"] = "Alice B."; // renamed during the call
  f.dir.photos["alice"] = "/p/alice-2.jpg";
  f.Fill(members, kT0 + 5000);
  EXPECT_EQ(f.dir.PhotoReads(), 2) << "5 s: once each";
  EXPECT_EQ(f.dir.NameReads(), 1);
  EXPECT_EQ(f.roster[1].name, "Alice B.");
  EXPECT_EQ(f.roster[1].avatar_src, "/p/alice-2.jpg");
  f.Fill(members, kT0 + 5016);
  EXPECT_EQ(f.dir.PhotoReads(), 2) << "and kept again from there";
}

TEST(CallRosterCacheTest, AnEmptyPhotoOrAPlaceholderNameIsKeptForFiveSecondsToo) {
  Fixture f;
  f.dir.photos.erase("bob");     // his photo file is not downloaded yet
  f.dir.names.erase("stranger"); // nobody we know
  const std::vector<CallScreen::Member> members = {Member("me", true), Member("bob"), Member("stranger")};
  f.Fill(members, kT0);
  EXPECT_FALSE(f.roster[1].has_avatar);
  EXPECT_TRUE(f.roster[2].avatar_unknown);
  EXPECT_EQ(f.roster[2].name, MakeCallerName("").label);
  f.dir.photo_reads.clear();
  f.dir.name_reads.clear();
  f.dir.photos["bob"] = "/p/bob.jpg"; // downloaded meanwhile
  f.dir.names["stranger"] = "Dave";   // saved as a contact meanwhile
  for (int64_t t = 16; t < 5000; t += 16) {
    f.Fill(members, kT0 + t);
  }
  EXPECT_EQ(f.dir.PhotoReads(), 0) << "nothing found is not read from disk every frame either";
  EXPECT_EQ(f.dir.NameReads(), 0);
  EXPECT_FALSE(f.roster[1].has_avatar);
  f.Fill(members, kT0 + 5000);
  EXPECT_EQ(f.dir.photo_reads, (std::map<std::string, int>{{"me", 1}, {"bob", 1}, {"stranger", 1}}));
  EXPECT_EQ(f.dir.name_reads, (std::map<std::string, int>{{"bob", 1}, {"stranger", 1}}));
  EXPECT_EQ(f.roster[1].avatar_src, "/p/bob.jpg") << "found at the 5 s re-read";
  EXPECT_EQ(f.roster[2].name, "Dave");
  EXPECT_FALSE(f.roster[2].avatar_unknown);
}

TEST(CallRosterCacheTest, TheCallStateFieldsAreTheScreensEveryFrame) {
  Fixture f;
  std::vector<CallScreen::Member> members = {Member("me", true), Member("alice")};
  f.Fill(members, kT0);
  EXPECT_FALSE(f.roster[1].audio_muted);
  members[1].audio_muted = true;
  members[1].video_enabled = true;
  members[1].on_hold = true;
  members[1].video_paused = true;
  members[1].guest = true;
  members[0].audio_muted = true;
  f.Fill(members, kT0 + 16);
  EXPECT_TRUE(f.roster[1].audio_muted);
  EXPECT_TRUE(f.roster[1].video_enabled);
  EXPECT_TRUE(f.roster[1].on_hold);
  EXPECT_TRUE(f.roster[1].video_paused);
  EXPECT_TRUE(f.roster[1].guest);
  EXPECT_TRUE(f.roster[0].audio_muted);
  EXPECT_TRUE(f.roster[0].is_local);
  EXPECT_FALSE(f.roster[1].is_local);
  members[1].audio_muted = false;
  f.Fill(members, kT0 + 32);
  EXPECT_FALSE(f.roster[1].audio_muted);
  EXPECT_EQ(f.roster[1].identity, "alice");
  EXPECT_EQ(f.roster[1].stream_id, std::to_string(PublisherStreamIdForIdentity("alice")));
  const AvatarGlyph glyph = MakeAvatarGlyph("Alice", "alice");
  EXPECT_EQ(f.roster[1].avatar_letter, glyph.letter);
  EXPECT_EQ(f.roster[1].avatar_tone, glyph.tone);
}

TEST(CallRosterCacheTest, TheCacheIsEmptiedWhenTheCallEndsOrAnotherBegins) {
  Fixture f;
  f.Fill({Member("me", true), Member("alice")}, kT0);
  EXPECT_GT(f.cache.Size(), 0u);
  f.cache.Clear(); // the call ended (CallController::ClearInCall)
  EXPECT_EQ(f.cache.Size(), 0u);
  f.Fill({Member("me", true), Member("alice")}, kT0 + 16);
  EXPECT_GT(f.cache.Size(), 0u);
  f.cache.ForCall("call-2");
  EXPECT_EQ(f.cache.Size(), 0u) << "another call starts from nothing";
}

TEST(CallRosterCacheTest, NoIdentityIsNeverLookedUpOrKept) {
  Fixture f;
  const CallRosterCache::Lookups lookups = f.dir.Lookups();
  f.cache.ForCall("call-1");
  EXPECT_EQ(f.cache.Icon("", kT0, lookups), "");
  EXPECT_EQ(f.dir.PhotoReads(), 0);
  EXPECT_EQ(f.cache.Size(), 0u);
}

} // namespace
} // namespace pbr
