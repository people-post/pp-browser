// The local "operate the app" renderer fed with what the REAL messaging tools return (registered on a real registry; no devices).
#include "feature/conversations/ConversationsFacade.h"
#include "feature/conversations/ConversationsHub.h"
#include "feature/conversations/MessagingTools.h"
#include "foundation/i18n/LocalizationService.h"
#include "lib/ai/LocalActionRender.h"
#include "lib/people/ModelFacingJson.h"

#include "common/ValueJson.h"

#include <gtest/gtest.h>

TEST(LocalActionRenderMessagingTest, OpenAndStartConversationRenderTheContactName) {
  ASSERT_TRUE(pbr::LocalizationService::Instance().LoadFromAssets(PP_BROWSER_ASSETS_DIR));
  pbr::LocalizationService::Instance().SetPreferredLanguage("en");

  pbr::ConversationsHub hub;
  pbr::ConversationsFacade facade(hub);
  pbr::ToolRegistry registry;
  pbr::RegisterMessagingTools(registry, facade);

  // Both tools return ThreadForModel(thread) on success.
  pbr::Thread thread;
  thread.id = "t1";
  const pbr::Object result = pbr::ThreadForModel(thread);
  pbr::LocalRenderContext ctx;
  ctx.name = "Old Wang";
  for (const char* tool : {"open_conversation", "start_conversation"}) {
    const std::string text = pbr::RenderLocalResult(tool, result, ctx);
    EXPECT_EQ(text, "Opened the chat with Old Wang.");
  }

  // Executing them needs a started hub (OpenThread on an idle hub crashes), so only their presence is checked.
  size_t found = 0;
  for (const auto& tool : registry.Tools()) {
    found += tool.definition.name == "open_conversation" || tool.definition.name == "start_conversation" ? 1 : 0;
  }
  EXPECT_EQ(found, 2u);

  // Readers that return arrays are people-list blocks elsewhere, not a card.
  EXPECT_TRUE(pbr::RenderLocalResult("list_contacts", pbr::Object{}, {}).empty());
}
