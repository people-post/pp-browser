#include "domain/ai/StructuredTextParser.h"

#include "common/PlatformLimits.h"

#include "common/chat/MessagingLimits.h"

#include <gtest/gtest.h>

#include <string>

TEST(StructuredTextParserTest, ComprehensiveParsingScenarios) {
  const std::string valid = R"({
    "blocks": [
      { "type": "paragraph", "text": "Hello <world> & \"quotes\"" },
      { "type": "heading", "level": 2, "text": "Title" },
      { "type": "list", "ordered": false, "items": ["A", "B"] },
      { "type": "code", "text": "int x = 1;" }
    ]
  })";

  auto result = pbr::StructuredTextParser::ParseBlocksJson(valid);
  ASSERT_TRUE(result.ok);
  ASSERT_TRUE(result.rml.find("assistant-message") == std::string::npos);
  ASSERT_TRUE(result.rml.find("selectable=\"text\"") == std::string::npos);
  ASSERT_TRUE(result.rml.find("<div class=\"stack\">") != std::string::npos);
  ASSERT_TRUE(result.chat_actions.empty());
  ASSERT_TRUE(result.warnings.empty());
  ASSERT_TRUE(result.rml.find("&lt;world&gt;") != std::string::npos);
  ASSERT_TRUE(result.rml.find("&amp;") != std::string::npos);
  ASSERT_TRUE(result.rml.find("&quot;") != std::string::npos);
  ASSERT_TRUE(result.rml.find("<h2>Title</h2>") != std::string::npos);
  ASSERT_TRUE(result.rml.find("<ul>") != std::string::npos);
  ASSERT_TRUE(result.rml.find("code-block") != std::string::npos);
  const std::string bad_type = R"({"blocks":[{"type":"table","text":"x"}]})";
  auto bad_type_result = pbr::StructuredTextParser::ParseBlocksJson(bad_type);
  ASSERT_TRUE(!bad_type_result.ok);
  ASSERT_TRUE(bad_type_result.rml.find("error") != std::string::npos);
  const std::string unknown_only = R"({"blocks":[{"type":"table","text":"x"},{"type":"card","title":"X"}]})";
  auto unknown_only_result = pbr::StructuredTextParser::ParseBlocksJson(unknown_only);
  ASSERT_TRUE(!unknown_only_result.ok);
  const std::string mixed = R"({
    "blocks": [
      { "type": "paragraph", "text": "Good" },
      { "type": "table", "text": "bad" }
    ]
  })";
  auto mixed_result = pbr::StructuredTextParser::ParseBlocksJson(mixed);
  ASSERT_TRUE(mixed_result.ok);
  ASSERT_TRUE(mixed_result.rml.find("<p>Good</p>") != std::string::npos);
  ASSERT_TRUE(mixed_result.warnings.size() == 1);
  ASSERT_TRUE(mixed_result.rml.find("Some blocks could not be displayed") != std::string::npos);
  const std::string bad_json = "not json";
  auto bad_json_result = pbr::StructuredTextParser::ParseBlocksJson(bad_json);
  ASSERT_TRUE(!bad_json_result.ok);
  const std::string llm_output = "Here is the answer:\n```json\n{\"blocks\":[{\"type\":\"paragraph\",\"text\":\"Hi\"}]}\n```";
  auto llm_result = pbr::StructuredTextParser::ParseFromLlmOutput(llm_output);
  ASSERT_TRUE(llm_result.ok);
  ASSERT_TRUE(llm_result.rml.find("<p>Hi</p>") != std::string::npos);
  const std::string bare_json = R"({
    "blocks": [
      { "type": "paragraph", "text": "Bare JSON works" }
    ]
  })";
  auto bare_result = pbr::StructuredTextParser::ParseFromLlmOutput(bare_json);
  ASSERT_TRUE(bare_result.ok);
  ASSERT_TRUE(bare_result.rml.find("Bare JSON works") != std::string::npos);
  const std::string embedded_tools = R"(```json
{
  "blocks": [
    {
      "tool": "web_search",
      "params": {
        "query": "today's stock market summary"
      }
    }
  ]
}
```)";
  auto embedded = pbr::StructuredTextParser::ExtractEmbeddedToolCalls(embedded_tools);
  ASSERT_TRUE(embedded.has_value());
  ASSERT_TRUE(embedded->size() == 1);
  ASSERT_TRUE(embedded->at(0).name == "web_search");
  ASSERT_TRUE(embedded->at(0).arguments.getString("query") == std::optional<std::string>("today's stock market summary"));
  const std::string embedded_parameters = R"(```json
{
  "blocks": [
    {
      "tool": "web_search",
      "parameters": {
        "query": "latest world news"
      }
    }
  ]
}
```)";
  auto embedded_parameters_result = pbr::StructuredTextParser::ExtractEmbeddedToolCalls(embedded_parameters);
  ASSERT_TRUE(embedded_parameters_result.has_value());
  ASSERT_TRUE(embedded_parameters_result->at(0).name == "web_search");
  ASSERT_TRUE(embedded_parameters_result->at(0).arguments.getString("query") == std::optional<std::string>("latest world news"));
  const std::string embedded_type = R"(```json
{
  "blocks": [
    {
      "type": "web_search",
      "query": "latest news on artificial intelligence"
    }
  ]
}
```)";
  auto embedded_type_result = pbr::StructuredTextParser::ExtractEmbeddedToolCalls(embedded_type);
  ASSERT_TRUE(embedded_type_result.has_value());
  ASSERT_TRUE(embedded_type_result->at(0).name == "web_search");
  ASSERT_TRUE(embedded_type_result->at(0).arguments.getString("query") == std::optional<std::string>("latest news on artificial intelligence"));
  auto embedded_parse = pbr::StructuredTextParser::ParseFromLlmOutput(embedded_tools);
  ASSERT_TRUE(!embedded_parse.ok);
  const std::string heading_clamp = R"({"blocks":[{"type":"heading","level":6,"text":"Big"}]})";
  auto heading_clamp_result = pbr::StructuredTextParser::ParseBlocksJson(heading_clamp);
  ASSERT_TRUE(!heading_clamp_result.ok);
  const std::string button_block = R"({
    "blocks": [
      { "type": "button", "label": "Say \"hi\"", "message": "It's a \\test" }
    ]
  })";
  auto button_result = pbr::StructuredTextParser::ParseBlocksJson(button_block);
  ASSERT_TRUE(button_result.ok);
  ASSERT_TRUE(button_result.rml.find("chat-suggestion") != std::string::npos);
  ASSERT_TRUE(button_result.rml.find("send_chat_action('__ENTRY__', 0)") != std::string::npos);
  ASSERT_TRUE(button_result.rml.find("send_suggestion(") == std::string::npos);
  ASSERT_TRUE(button_result.chat_actions.size() == 1);
  ASSERT_TRUE(button_result.chat_actions[0].label == "Say \"hi\"");
  ASSERT_TRUE(button_result.chat_actions[0].message == "It's a \\test");
  ASSERT_TRUE(!button_result.chat_actions[0].payload.has_value());
  const std::string button_with_payload = R"({
    "blocks": [
      {
        "type": "button",
        "label": "Pick A",
        "message": "I choose A",
        "payload": "{\"type\":\"choice\",\"value\":\"A\"}"
      }
    ]
  })";
  auto payload_button = pbr::StructuredTextParser::ParseBlocksJson(button_with_payload);
  ASSERT_TRUE(payload_button.ok);
  ASSERT_TRUE(payload_button.chat_actions.size() == 1);
  ASSERT_TRUE(payload_button.chat_actions[0].payload.has_value());
  ASSERT_TRUE(payload_button.chat_actions[0].payload->find("choice") != std::string::npos);
  const std::string multi_button = R"({
    "blocks": [
      { "type": "button", "label": "One", "message": "First" },
      { "type": "button", "label": "Two", "message": "Second" }
    ]
  })";
  auto multi_button_result = pbr::StructuredTextParser::ParseBlocksJson(multi_button);
  ASSERT_TRUE(multi_button_result.ok);
  ASSERT_TRUE(multi_button_result.chat_actions.size() == 2);
  ASSERT_TRUE(multi_button_result.rml.find("send_chat_action('__ENTRY__', 0)") != std::string::npos);
  ASSERT_TRUE(multi_button_result.rml.find("send_chat_action('__ENTRY__', 1)") != std::string::npos);
  const std::string button_missing_message = R"({"blocks":[{"type":"button","label":"Go"}]})";
  auto button_missing_message_result = pbr::StructuredTextParser::ParseBlocksJson(button_missing_message);
  ASSERT_TRUE(!button_missing_message_result.ok);
  const std::string button_empty_message = R"({"blocks":[{"type":"button","label":"Go","message":""}]})";
  auto button_empty_message_result = pbr::StructuredTextParser::ParseBlocksJson(button_empty_message);
  ASSERT_TRUE(!button_empty_message_result.ok);
  const std::string button_only_invalid = R"({"blocks":[{"type":"button","label":"Go"}]})";
  auto button_only_invalid_result = pbr::StructuredTextParser::ParseBlocksJson(button_only_invalid);
  ASSERT_TRUE(!button_only_invalid_result.ok);
  // LLMs often omit the final closing brace on the root object.
  const std::string missing_root_brace = R"({
    "blocks": [
      { "type": "paragraph", "text": "Hi" },
      { "type": "button", "label": "More", "message": "Tell me more" }
    ]
  )";
  auto repaired_result = pbr::StructuredTextParser::ParseBlocksJson(missing_root_brace);
  ASSERT_TRUE(repaired_result.ok);
  ASSERT_TRUE(repaired_result.rml.find("<p>Hi</p>") != std::string::npos);
  ASSERT_TRUE(repaired_result.rml.find("chat-suggestion") != std::string::npos);
  ASSERT_TRUE(repaired_result.rml.find("selectable=\"text\"") == std::string::npos);
  ASSERT_TRUE(repaired_result.chat_actions.size() == 1);
  ASSERT_TRUE(repaired_result.chat_actions[0].label == "More");
  ASSERT_TRUE(repaired_result.chat_actions[0].message == "Tell me more");
  const std::string form_block = R"({
    "blocks": [
      { "type": "form", "id": "booking", "title": "Book", "submit_label": "Go",
        "submit_template": "Book {{name}}",
        "fields": [{ "id": "name", "label": "Name", "field_type": "text" }] }
    ]
  })";
  auto form_result = pbr::StructuredTextParser::ParseBlocksJson(form_block);
  ASSERT_TRUE(form_result.ok);
  ASSERT_TRUE(form_result.widget_inits.size() == 1);
  ASSERT_TRUE(form_result.working_set_candidates.size() == 1);
  ASSERT_TRUE(form_result.rml.find("chat-working-set-chip") != std::string::npos);
  ASSERT_TRUE(form_result.rml.find("open_working_set('__ENTRY__', 0)") != std::string::npos);
  ASSERT_TRUE(form_result.rml.find("data-value=\"field.value\"") == std::string::npos);
  ASSERT_TRUE(form_result.working_set_candidates[0].artifact_rml.find("working-set-form") != std::string::npos);
  ASSERT_TRUE(form_result.working_set_candidates[0].artifact_rml.find("submit_form('__ENTRY__', 'booking')") !=
         std::string::npos);

  const std::string calendar_block = R"({
    "blocks": [
      { "type": "calendar", "month": 6, "year": 2026, "available_days": ["2026-06-15"] }
    ]
  })";
  auto calendar_result = pbr::StructuredTextParser::ParseBlocksJson(calendar_block);
  ASSERT_TRUE(calendar_result.ok);
  ASSERT_TRUE(calendar_result.widget_inits.size() == 1);
  ASSERT_TRUE(calendar_result.working_set_candidates.size() == 1);
  ASSERT_TRUE(calendar_result.rml.find("chat-working-set-chip") != std::string::npos);
  ASSERT_TRUE(calendar_result.working_set_candidates[0].artifact_rml.find("working-set-calendar") != std::string::npos);
  ASSERT_TRUE(calendar_result.working_set_candidates[0].artifact_rml.find("calendar_prev('__ENTRY__')") != std::string::npos);
  ASSERT_TRUE(calendar_result.working_set_candidates[0].artifact_rml.find("working_set.calendar.weeks") != std::string::npos);
  ASSERT_TRUE(calendar_result.working_set_candidates[0].artifact_rml.find("<table class=\"calendar-grid\">") !=
         std::string::npos);

  const std::string calendar_defaults = R"({"blocks":[{"type":"calendar"}]})";
  auto calendar_defaults_result = pbr::StructuredTextParser::ParseBlocksJson(calendar_defaults);
  ASSERT_TRUE(calendar_defaults_result.ok);
  ASSERT_TRUE(calendar_defaults_result.widget_inits.size() == 1);
  const std::string card_block = R"({
    "blocks": [
      { "type": "card", "title": "Title", "body": "Body text" }
    ]
  })";
  auto card_result = pbr::StructuredTextParser::ParseBlocksJson(card_block);
  ASSERT_TRUE(card_result.ok);
  ASSERT_TRUE(card_result.rml.find("chat-card") != std::string::npos);
  const std::string poll_block = R"({
    "blocks": [
      { "type": "poll", "question": "Pick one", "options": [
        { "label": "A", "message": "Choose A" }
      ]}
    ]
  })";
  auto poll_result = pbr::StructuredTextParser::ParseBlocksJson(poll_block);
  ASSERT_TRUE(poll_result.ok);
  ASSERT_TRUE(poll_result.chat_actions.size() == 1);
  ASSERT_TRUE(poll_result.rml.find("chat-poll") != std::string::npos);
  const std::string long_list_block = R"({
    "blocks": [
      {
        "type": "long_list",
        "title": "Articles from Brief",
        "items": [
          {
            "id": "art-001",
            "title": "Market outlook <Q2>",
            "subtitle": "Cross-asset view & \"rates\"",
            "meta": "2025-06-10",
            "actions": [
              { "label": "Summarize", "message": "Summarize art-001" },
              {
                "label": "Open",
                "message": "Open art-001",
                "payload": { "type": "article", "id": "art-001" }
              }
            ]
          }
        ],
        "footer_actions": [
          {
            "label": "More",
            "message": "Load more",
            "payload": { "tool": "blog_articles", "before_id": "art-001", "size": 10 }
          }
        ]
      }
    ]
  })";
  auto long_list_result = pbr::StructuredTextParser::ParseBlocksJson(long_list_block, pbr::ResponseGoal::PeopleDiscovery);
  ASSERT_TRUE(long_list_result.ok);
  ASSERT_TRUE(long_list_result.working_set_candidates.size() == 1);
  ASSERT_TRUE(long_list_result.rml.find("chat-working-set-chip") != std::string::npos);
  ASSERT_TRUE(long_list_result.rml.find("chat-long-list-scroll") == std::string::npos);
  ASSERT_TRUE(long_list_result.working_set_candidates[0].artifact_rml.find("working-set-long-list") != std::string::npos);
  ASSERT_TRUE(long_list_result.working_set_candidates[0].artifact_rml.find("chat-long-list-title") != std::string::npos);
  ASSERT_TRUE(long_list_result.working_set_candidates[0].artifact_rml.find("&lt;Q2&gt;") != std::string::npos);
  ASSERT_TRUE(long_list_result.working_set_candidates[0].artifact_rml.find("&amp;") != std::string::npos);
  ASSERT_TRUE(long_list_result.working_set_candidates[0].artifact_rml.find("chat-long-list-footer") != std::string::npos);
  ASSERT_TRUE(long_list_result.chat_actions.size() == 3);
  const std::string paragraph_only = R"({"blocks":[{"type":"paragraph","text":"Hello"}]})";
  auto paragraph_only_result = pbr::StructuredTextParser::ParseBlocksJson(paragraph_only);
  ASSERT_TRUE(paragraph_only_result.ok);
  ASSERT_TRUE(paragraph_only_result.working_set_candidates.empty());
  const std::string long_list_missing_items = R"({"blocks":[{"type":"long_list","title":"X"}]})";
  auto long_list_missing_items_result = pbr::StructuredTextParser::ParseBlocksJson(long_list_missing_items);
  ASSERT_TRUE(!long_list_missing_items_result.ok);
}

TEST(StructuredTextParserTest, BoundsRenderedRmlAtConfiguredLimit) {
  const std::string empty_paragraph = R"({"blocks":[{"type":"paragraph","text":""}]})";
  const auto empty_result = pbr::StructuredTextParser::ParseBlocksJson(empty_paragraph);
  ASSERT_TRUE(empty_result.ok);

  const size_t text_bytes = pbr::kMaxStructuredParserOutputBytes - empty_result.rml.size();
  const auto at_limit = pbr::StructuredTextParser::ParseBlocksJson(
      R"({"blocks":[{"type":"paragraph","text":")" + std::string(text_bytes, 'x') + R"("}]})");
  ASSERT_TRUE(at_limit.ok);
  EXPECT_EQ(at_limit.rml.size(), pbr::kMaxStructuredParserOutputBytes);

  const auto over_limit = pbr::StructuredTextParser::ParseBlocksJson(
      R"({"blocks":[{"type":"paragraph","text":")" + std::string(text_bytes + 1, 'x') + R"("}]})");
  ASSERT_FALSE(over_limit.ok);
  EXPECT_EQ(over_limit.error,
            "Structured parser output exceeds limit of " + std::to_string(pbr::kMaxStructuredParserOutputBytes) +
                " bytes");
}

TEST(StructuredTextParserTest, PlainTextReadsFencedAndBareBlocksAndPassesProseThrough) {
  const std::string fenced = "```json { \"blocks\": [ { \"type\": \"paragraph\", \"text\": \"你好！很高兴见到你。\" } ] } ```";
  EXPECT_EQ(pbr::StructuredTextParser::PlainText(fenced), "你好！很高兴见到你。");

  const std::string bare = R"({"blocks":[
    {"type":"heading","level":2,"text":"Title"},
    {"type":"paragraph","text":"Body."},
    {"type":"list","items":["one","two"]},
    {"type":"card","title":"Card","body":"Card body"},
    {"type":"key_value","items":[{"label":"Price","value":"$1"}]},
    {"type":"button","label":"Go","message":"go"}
  ]})";
  EXPECT_EQ(pbr::StructuredTextParser::PlainText(bare), "Title\nBody.\n- one\n- two\nCard\nCard body\n- Price: $1");

  EXPECT_EQ(pbr::StructuredTextParser::PlainText("plain prose"), "plain prose");
  EXPECT_EQ(pbr::StructuredTextParser::PlainText(R"({"blocks":[{"type":"button","label":"x"}]})"),
            R"({"blocks":[{"type":"button","label":"x"}]})");

  // Callers that must never forward JSON tell "prose" from "blocks with nothing readable".
  EXPECT_FALSE(pbr::StructuredTextParser::PlainTextIfBlocks("plain prose").has_value());
  EXPECT_EQ(pbr::StructuredTextParser::PlainTextIfBlocks(R"({"blocks":[{"type":"button","label":"x"}]})"), "");

  // Tables and poll options read as lines; a truncated document gets the renderer's brace repair.
  EXPECT_EQ(pbr::StructuredTextParser::PlainText(
                R"({"blocks":[{"type":"table","headers":["A","B"],"rows":[["1","2"]]},{"type":"poll","question":"Q?","options":["yes","no"]}]})"),
            "A | B\n1 | 2\nQ?\n- yes\n- no");
  EXPECT_EQ(pbr::StructuredTextParser::PlainText(R"({"blocks":[{"type":"paragraph","text":"cut off"}])"), "cut off");
}

TEST(StructuredTextParserTest, StorableTextKeepsSmallAnswersAndReducesHugeOnesToProse) {
  const std::string small = R"({"blocks":[{"type":"paragraph","text":"Found 2 people:"}]})";
  EXPECT_EQ(pbr::StructuredTextParser::StorableText(small), small);

  // A people list whose action payloads carry large public keys.
  const std::string huge = R"({"blocks":[{"type":"paragraph","text":"Found 9 people:"},{"type":"long_list","title":"Search results","items":[{"title":"A","key":")" +
                           std::string(pbr::kMaxComposeTextBytes + 100, 'k') + R"("}]}]})";
  const std::string stored = pbr::StructuredTextParser::StorableText(huge);
  EXPECT_LE(stored.size(), pbr::kMaxComposeTextBytes);
  EXPECT_NE(stored.find("Found 9 people:"), std::string::npos);
  EXPECT_EQ(stored.find("kkkk"), std::string::npos);

  // Plain text that is too long is cut on a character boundary.
  std::string cjk;
  while (cjk.size() <= pbr::kMaxComposeTextBytes) {
    cjk += "好";
  }
  const std::string cut = pbr::StructuredTextParser::StorableText(cjk);
  EXPECT_LE(cut.size(), pbr::kMaxComposeTextBytes);
  EXPECT_EQ(cut.size() % 3, 0u);
}

TEST(StructuredTextParserTest, ListBlockDrawsItsOwnMarkers) {
  // The engine draws no list markers; the block uses the same marker markup as Markdown lists.
  auto bullets = pbr::StructuredTextParser::ParseBlocksJson(R"({"blocks":[{"type":"list","items":["A","B"]}]})");
  ASSERT_TRUE(bullets.ok);
  EXPECT_NE(bullets.rml.find("<ul><li><span class=\"md-marker\">\xE2\x80\xA2</span><div class=\"md-item\">A</div></li>"),
            std::string::npos);

  auto numbered =
      pbr::StructuredTextParser::ParseBlocksJson(R"({"blocks":[{"type":"list","ordered":true,"items":["A","B"]}]})");
  ASSERT_TRUE(numbered.ok);
  EXPECT_NE(numbered.rml.find("<li><span class=\"md-marker\">2.</span><div class=\"md-item\">B</div></li></ol>"),
            std::string::npos);
}

// PB-016: the form id lands in data-form-id="…" and submit_form('…') verbatim, so it must be a plain token.
TEST(StructuredTextParserTest, FormIdMustBeAPlainToken) {
  const auto block = [](const std::string& id) {
    return std::string(R"({"blocks":[{"type":"form","id":")") + id +
           R"(","title":"Book","submit_label":"Go","submit_template":"book {{when}}","fields":[{"id":"when","label":"When","field_type":"date"}]}]})";
  };
  for (const char* bad : {"a'); evil('", "a>b", "{{x}}", "", "with space"}) {
    const auto result = pbr::StructuredTextParser::ParseBlocksJson(block(bad));
    EXPECT_FALSE(result.ok) << "accepted form id: " << bad;
    EXPECT_TRUE(result.widget_inits.empty()) << bad;
    EXPECT_EQ(result.rml.find("data-form-id"), std::string::npos) << bad;
  }
  EXPECT_FALSE(pbr::StructuredTextParser::ParseBlocksJson(block(std::string(65, 'a'))).ok);

  // In the chat the form renders as a working-set chip; the id is substituted into the panel's markup.
  for (const char* good : {"booking", "Booking-2", "form_1", "a"}) {
    const auto result = pbr::StructuredTextParser::ParseBlocksJson(block(good));
    EXPECT_TRUE(result.ok) << "rejected form id: " << good;
    EXPECT_EQ(result.widget_inits.size(), 1u) << good;
  }
  EXPECT_TRUE(pbr::StructuredTextParser::ParseBlocksJson(block(std::string(64, 'a'))).ok);
}

// PB-007: a payload in model output must not reach a privileged local action (add_contact can replace a
// peer's keys). From the model only `open_url` and tool payloads survive; locally built blocks keep theirs.
TEST(StructuredTextParserTest, ModelOutputMayNotCarryPrivilegedPayloads) {
  const std::string blocks = R"({"blocks":[
    {"type":"button","label":"Add","message":"add","payload":{"type":"add_contact","directory_hit":{"hit_id":"h","account_id":"account:x"}}},
    {"type":"button","label":"Open","message":"open","payload":{"type":"open_url","url":"https://example.com/"}},
    {"type":"button","label":"More","message":"more","payload":{"tool":"blog_articles","args":{"page":2}}},
    {"type":"action_list","items":[{"title":"t","actions":[{"label":"Reg","message":"reg","payload":{"type":"register_user"}}]}]},
    {"type":"choice","prompt":"p","options":[{"label":"Create","message":"create","payload":{"type":"create_group"}}]},
    {"type":"poll","question":"q","options":[{"label":"Fork","message":"fork","payload":{"type":"fork_group"}}]}
  ]})";
  const auto payload_of = [](const pbr::ParseResult& r, const std::string& label) -> std::optional<std::string> {
    for (const auto& action : r.chat_actions) {
      if (action.label == label) {
        return action.payload;
      }
    }
    return std::string("<no action>");
  };

  const auto model = pbr::StructuredTextParser::ParseFromLlmOutput("```json\n" + blocks + "\n```");
  ASSERT_TRUE(model.ok) << model.error;
  EXPECT_FALSE(payload_of(model, "Add").has_value());
  EXPECT_FALSE(payload_of(model, "Reg").has_value());
  EXPECT_FALSE(payload_of(model, "Create").has_value());
  EXPECT_FALSE(payload_of(model, "Fork").has_value());
  EXPECT_TRUE(payload_of(model, "Open").has_value());
  EXPECT_TRUE(payload_of(model, "More").has_value());
  bool warned = false;
  for (const std::string& warning : model.warnings) {
    warned = warned || warning.find("add_contact") != std::string::npos;
  }
  EXPECT_TRUE(warned) << "dropping a payload should leave a warning";

  const auto local = pbr::StructuredTextParser::ParseBlocksJson(blocks);
  ASSERT_TRUE(local.ok) << local.error;
  for (const char* label : {"Add", "Open", "More", "Reg", "Create", "Fork"}) {
    EXPECT_TRUE(payload_of(local, label).has_value()) << label;
  }
  EXPECT_TRUE(local.warnings.empty());
}
