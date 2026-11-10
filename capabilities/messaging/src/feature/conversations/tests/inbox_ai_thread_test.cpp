// The PP助手 page's swipe menu on an AI conversation: Rename keeps the user's own name (it survives a restart and an
// empty name goes back to the automatic one), Delete takes the thread and its memory with it, Pin is stored.
#include "lib/messaging/testing/InMemoryMessagingStores.h"
#include "lib/delivery/testing/InMemoryDeliveryStore.h"
#include "lib/net/testing/MockOrgBackendClients.h"
#include "lib/people/ContactsStore.h"
#include "domain/peers/DirectoryShadowCache.h"
#include "feature/conversations/InboxController.h"
#include "feature/conversations/PeerDisplaySources.h"

#include <algorithm>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <stdexcept>

namespace pbr {
namespace {

ByteVector TestDek() {
  ByteVector dek(32);
  for (size_t i = 0; i < dek.size(); ++i) {
    dek[i] = static_cast<uint8_t>(0xb0 + i);
  }
  return dek;
}

struct AiInboxEnv {
  AiInboxEnv() {
    data_dir = std::filesystem::temp_directory_path() / "pp_browser_inbox_ai_thread";
    std::filesystem::remove_all(data_dir);
    store = std::make_unique<InMemoryDeliveryStore>();
    if (!store->SetDek(TestDek())) {
      throw std::runtime_error("Failed to set test DEK");
    }
    contacts = std::make_unique<ContactsStore>(data_dir.string());
    shadows = std::make_unique<DirectoryShadowCache>(directory);
    labels = std::make_unique<PeerDisplayResolver>(*contacts, PeerDisplaySourcesFrom(*shadows));
    Reopen();
  }

  /** A restart: a new controller over the same stored threads. */
  void Reopen() { inbox = std::make_unique<InboxController>(*store, *contacts, *labels, shadows.get()); }

  void AddAiThread(const std::string& id, const std::string& title) {
    Thread thread;
    thread.id = id;
    thread.kind = ThreadKind::Ai;
    thread.title = title;
    thread.updated_at = 1;
    ASSERT_TRUE(store->UpsertThread(thread));
  }

  std::string TitleOf(const std::string& id) {
    auto thread = store->GetThread(id);
    if (!thread || !*thread) {
      return "<missing>";
    }
    return inbox->ResolveThreadLabel(**thread).title;
  }

  bool Listed(const std::string& id) {
    auto threads = inbox->ListThreads();
    return threads && std::any_of(threads->begin(), threads->end(), [&](const Thread& t) { return t.id == id; });
  }

  std::filesystem::path data_dir;
  MockDirectoryClient directory;
  std::unique_ptr<InMemoryDeliveryStore> store;
  std::unique_ptr<ContactsStore> contacts;
  std::unique_ptr<DirectoryShadowCache> shadows;
  std::unique_ptr<PeerDisplayResolver> labels;
  std::unique_ptr<InboxController> inbox;
};

TEST(InboxAiThreadTest, RenameShowsTheOwnNameAndSurvivesARestart) {
  AiInboxEnv env;
  env.AddAiThread("ai-1", "Plan a trip");
  EXPECT_EQ(env.TitleOf("ai-1"), "Plan a trip");

  ASSERT_TRUE(env.inbox->SetThreadLocalTitle("ai-1", "Japan 2027"));
  EXPECT_EQ(env.TitleOf("ai-1"), "Japan 2027");

  env.Reopen();
  EXPECT_EQ(env.TitleOf("ai-1"), "Japan 2027") << "the name is stored, not held in the screen";
  EXPECT_TRUE(env.Listed("ai-1"));
}

TEST(InboxAiThreadTest, AnEmptyNameGoesBackToTheAutomaticTitle) {
  AiInboxEnv env;
  env.AddAiThread("ai-1", "Plan a trip");
  ASSERT_TRUE(env.inbox->SetThreadLocalTitle("ai-1", "Japan 2027"));
  ASSERT_TRUE(env.inbox->SetThreadLocalTitle("ai-1", ""));
  EXPECT_EQ(env.TitleOf("ai-1"), "Plan a trip");
  env.Reopen();
  EXPECT_EQ(env.TitleOf("ai-1"), "Plan a trip");
}

TEST(InboxAiThreadTest, RenameIsRefusedForADirectChat) {
  AiInboxEnv env;
  Thread direct;
  direct.id = "d-1";
  direct.kind = ThreadKind::Direct;
  direct.title = "Peer";
  direct.updated_at = 1;
  ASSERT_TRUE(env.store->UpsertThread(direct));
  EXPECT_FALSE(env.inbox->SetThreadLocalTitle("d-1", "Mine"));
}

TEST(InboxAiThreadTest, DeleteRemovesTheThreadAndItsMemory) {
  AiInboxEnv env;
  env.AddAiThread("ai-1", "Plan a trip");
  ConversationSummary summary;
  summary.text = "The user plans a trip to Japan.";
  summary.version = 1;
  ASSERT_TRUE(env.store->SetThreadMemory("ai-1", summary));

  // The order the app's delete uses: the memory is cleared, then the thread closed.
  ASSERT_TRUE(env.inbox->ForgetThreadMemory("ai-1"));
  auto memory = env.store->GetThreadMemory("ai-1");
  ASSERT_TRUE(memory);
  EXPECT_FALSE(memory->has_value()) << "the AI no longer remembers the conversation";

  ASSERT_TRUE(env.inbox->CloseThread("ai-1"));
  EXPECT_FALSE(env.Listed("ai-1"));
  env.Reopen();
  EXPECT_FALSE(env.Listed("ai-1")) << "gone after a restart too";
}

TEST(InboxAiThreadTest, AMemoryOfADeletedThreadCannotBeClearedAgain) {
  AiInboxEnv env;
  env.AddAiThread("ai-1", "x");
  ASSERT_TRUE(env.inbox->CloseThread("ai-1"));
  EXPECT_FALSE(env.inbox->ForgetThreadMemory("ai-1")) << "nothing left to touch; the app ignores this result";
}

TEST(InboxAiThreadTest, ForgetMemoryRefusesAHumanChat) {
  AiInboxEnv env;
  Thread direct;
  direct.id = "d-1";
  direct.kind = ThreadKind::Direct;
  direct.title = "Peer";
  direct.updated_at = 1;
  ASSERT_TRUE(env.store->UpsertThread(direct));
  EXPECT_FALSE(env.inbox->ForgetThreadMemory("d-1")) << "the delete path only forgets AI conversations";
}

TEST(InboxAiThreadTest, ThePinOfAnAiThreadIsAStoredPreference) {
  InMemoryThreadLocalPrefsStore prefs;
  ThreadLocalPrefs pin;
  pin.thread_id = "ai-1";
  pin.pinned_at = 100;
  ASSERT_TRUE(prefs.Set(pin));
  auto got = prefs.Get("ai-1");
  ASSERT_TRUE(got);
  EXPECT_EQ(got->pinned_at, 100);
  pin.pinned_at = 0; // Unpin
  ASSERT_TRUE(prefs.Set(pin));
  EXPECT_EQ(prefs.Get("ai-1")->pinned_at, 0);
}

std::string StoredTitle(AiInboxEnv& env, const std::string& id) {
  auto thread = env.store->GetThread(id);
  return thread && *thread ? (*thread)->title : "<missing>";
}

std::string StoredPreview(AiInboxEnv& env, const std::string& id) {
  auto thread = env.store->GetThread(id);
  return thread && *thread ? (*thread)->preview : "<missing>";
}

TEST(InboxAiThreadTest, ALongFirstMessageIsCutOnACharacterBoundary) {
  AiInboxEnv env;
  env.AddAiThread("ai-1", kDefaultAiThreadTitle);
  std::string cjk;
  for (int i = 0; i < 40; ++i) {
    cjk += "一二三四";
  }
  ASSERT_TRUE(env.inbox->NameAiThreadFromFirstMessage("ai-1", cjk));
  std::string expected;
  for (int i = 0; i < 6; ++i) {
    expected += "一二三四"; // 24 characters, whole
  }
  EXPECT_EQ(StoredTitle(env, "ai-1"), expected + "…");
  env.Reopen();
  EXPECT_EQ(env.TitleOf("ai-1"), expected + "…") << "the title is stored";
}

TEST(InboxAiThreadTest, ATitleNeverSplitsAnEmojiSequence) {
  AiInboxEnv env;
  env.AddAiThread("ai-1", kDefaultAiThreadTitle);
  const std::string family =
      "\xF0\x9F\x91\xA8\xE2\x80\x8D\xF0\x9F\x91\xA9\xE2\x80\x8D\xF0\x9F\x91\xA7"; // man+woman+girl
  std::string text(23, 'a');
  text += family + "tail"; // the family is the 24th character
  ASSERT_TRUE(env.inbox->NameAiThreadFromFirstMessage("ai-1", text));
  EXPECT_EQ(StoredTitle(env, "ai-1"), std::string(23, 'a') + family + "…");
}

TEST(InboxAiThreadTest, AShortSentenceIsShownAsIs) {
  AiInboxEnv env;
  env.AddAiThread("ai-1", kDefaultAiThreadTitle);
  ASSERT_TRUE(env.inbox->NameAiThreadFromFirstMessage("ai-1", "  Plan a trip\n to Japan "));
  EXPECT_EQ(StoredTitle(env, "ai-1"), "Plan a trip to Japan");
  ASSERT_TRUE(env.inbox->UpdatePreview("ai-1", "Plan a trip to Japan"));
  EXPECT_EQ(StoredPreview(env, "ai-1"), "Plan a trip to Japan");
}

TEST(InboxAiThreadTest, ADeviceLinkCodeIsNeverTheTitleOrPreview) {
  AiInboxEnv env;
  env.AddAiThread("ai-1", kDefaultAiThreadTitle);
  const std::string code = "pp-browser-link-device-v1:AbCdEf0123456789secret";
  ASSERT_TRUE(env.inbox->NameAiThreadFromFirstMessage("ai-1", code));
  ASSERT_TRUE(env.inbox->UpdatePreview("ai-1", code));
  EXPECT_EQ(StoredTitle(env, "ai-1"), kDefaultAiThreadTitle);
  EXPECT_EQ(StoredPreview(env, "ai-1").find("secret"), std::string::npos);
  EXPECT_EQ(StoredPreview(env, "ai-1").find("pp-browser-link"), std::string::npos);
  EXPECT_FALSE(StoredPreview(env, "ai-1").empty());
}

TEST(InboxAiThreadTest, ABase64BlobOrOpaquePasteGetsThePlaceholder) {
  // A long base64 run (with a digit), an opaque ASCII paste with no spaces, and a base64 run inside other text.
  for (const std::string& text :
       {std::string("QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVowMTIzNDU2Nzg5"), std::string(250, 'x'),
        std::string("see ") + "QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVowMTIz"}) {
    AiInboxEnv env;
    env.AddAiThread("ai-1", kDefaultAiThreadTitle);
    ASSERT_TRUE(env.inbox->NameAiThreadFromFirstMessage("ai-1", text));
    ASSERT_TRUE(env.inbox->UpdatePreview("ai-1", text));
    EXPECT_EQ(StoredTitle(env, "ai-1"), kDefaultAiThreadTitle) << text;
    EXPECT_EQ(StoredPreview(env, "ai-1").find(text.substr(text.size() - 6)), std::string::npos) << text;
  }
}

TEST(InboxAiThreadTest, APlainNumberOrLongCjkIsNotMasked) {
  // A bare number (a year, a PIN, an amount) and ordinary prose — including long space-free Chinese — are not codes.
  std::string long_cjk;
  for (int i = 0; i < 250; ++i) {
    long_cjk += "中"; // 250 CJK codepoints, no spaces: prose, not a blob
  }
  for (const std::string& text : {std::string("123456"), std::string("2025"), long_cjk}) {
    AiInboxEnv env;
    env.AddAiThread("ai-1", kDefaultAiThreadTitle);
    ASSERT_TRUE(env.inbox->NameAiThreadFromFirstMessage("ai-1", text));
    ASSERT_TRUE(env.inbox->UpdatePreview("ai-1", text));
    EXPECT_NE(StoredTitle(env, "ai-1"), kDefaultAiThreadTitle) << text; // named from the message, not left default
    EXPECT_EQ(StoredPreview(env, "ai-1"), text) << "preview kept unmasked";
  }
}

TEST(InboxAiThreadTest, AnAssistantAnswerWithALongHashKeepsItsPreview) {
  AiInboxEnv env;
  env.AddAiThread("ai-1", kDefaultAiThreadTitle);
  const std::string answer = "The file hash is QUJDREVGR0hJSktMTU5PUFFSU1RVVldYWVowMTIzNDU2Nzg5 as asked";
  ASSERT_TRUE(env.inbox->UpdatePreview("ai-1", answer, /*assistant_text=*/true));
  EXPECT_EQ(StoredPreview(env, "ai-1"), answer);
}

TEST(InboxAiThreadTest, AHumanChatPreviewIsNotMasked) {
  AiInboxEnv env;
  Thread direct;
  direct.id = "d-1";
  direct.kind = ThreadKind::Direct;
  direct.title = "Peer";
  direct.updated_at = 1;
  ASSERT_TRUE(env.store->UpsertThread(direct));
  ASSERT_TRUE(env.inbox->UpdatePreview("d-1", "123456"));
  EXPECT_EQ(StoredPreview(env, "d-1"), "123456");
}

TEST(InboxAiThreadTest, ARenamedThreadKeepsItsOwnName) {
  AiInboxEnv env;
  env.AddAiThread("ai-1", kDefaultAiThreadTitle);
  ASSERT_TRUE(env.inbox->SetThreadLocalTitle("ai-1", "Japan 2027"));
  ASSERT_TRUE(env.inbox->NameAiThreadFromFirstMessage("ai-1", "Plan a trip"));
  EXPECT_EQ(env.TitleOf("ai-1"), "Japan 2027");
  env.Reopen();
  EXPECT_EQ(env.TitleOf("ai-1"), "Japan 2027");
}

} // namespace
} // namespace pbr
