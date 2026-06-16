#include "agent/StructuredTextParser.h"

#include <cassert>
#include <iostream>
#include <string>

int main() {
  const std::string valid = R"({
    "blocks": [
      { "type": "paragraph", "text": "Hello <world> & \"quotes\"" },
      { "type": "heading", "level": 2, "text": "Title" },
      { "type": "list", "ordered": false, "items": ["A", "B"] },
      { "type": "code", "text": "int x = 1;" }
    ]
  })";

  auto result = pbr::StructuredTextParser::ParseBlocksJson(valid);
  assert(result.ok);
  assert(result.rml.find("assistant-message") == std::string::npos);
  assert(result.rml.find("selectable=\"text\"") == std::string::npos);
  assert(result.rml.find("<div class=\"stack\">") != std::string::npos);
  assert(result.chat_actions.empty());
  assert(result.warnings.empty());
  assert(result.rml.find("&lt;world&gt;") != std::string::npos);
  assert(result.rml.find("&amp;") != std::string::npos);
  assert(result.rml.find("&quot;") != std::string::npos);
  assert(result.rml.find("<h2>Title</h2>") != std::string::npos);
  assert(result.rml.find("<ul>") != std::string::npos);
  assert(result.rml.find("code-block") != std::string::npos);

  const std::string bad_type = R"({"blocks":[{"type":"table","text":"x"}]})";
  auto bad_type_result = pbr::StructuredTextParser::ParseBlocksJson(bad_type);
  assert(!bad_type_result.ok);
  assert(bad_type_result.rml.find("error") != std::string::npos);

  const std::string unknown_only = R"({"blocks":[{"type":"table","text":"x"},{"type":"card","title":"X"}]})";
  auto unknown_only_result = pbr::StructuredTextParser::ParseBlocksJson(unknown_only);
  assert(!unknown_only_result.ok);

  const std::string mixed = R"({
    "blocks": [
      { "type": "paragraph", "text": "Good" },
      { "type": "table", "text": "bad" }
    ]
  })";
  auto mixed_result = pbr::StructuredTextParser::ParseBlocksJson(mixed);
  assert(mixed_result.ok);
  assert(mixed_result.rml.find("<p>Good</p>") != std::string::npos);
  assert(mixed_result.warnings.size() == 1);
  assert(mixed_result.rml.find("Some blocks could not be displayed") != std::string::npos);

  const std::string bad_json = "not json";
  auto bad_json_result = pbr::StructuredTextParser::ParseBlocksJson(bad_json);
  assert(!bad_json_result.ok);

  const std::string llm_output = "Here is the answer:\n```json\n{\"blocks\":[{\"type\":\"paragraph\",\"text\":\"Hi\"}]}\n```";
  auto llm_result = pbr::StructuredTextParser::ParseFromLlmOutput(llm_output);
  assert(llm_result.ok);
  assert(llm_result.rml.find("<p>Hi</p>") != std::string::npos);

  const std::string bare_json = R"({
    "blocks": [
      { "type": "paragraph", "text": "Bare JSON works" }
    ]
  })";
  auto bare_result = pbr::StructuredTextParser::ParseFromLlmOutput(bare_json);
  assert(bare_result.ok);
  assert(bare_result.rml.find("Bare JSON works") != std::string::npos);

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
  assert(embedded.has_value());
  assert(embedded->size() == 1);
  assert(embedded->at(0).name == "web_search");
  assert(embedded->at(0).arguments["query"] == "today's stock market summary");

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
  assert(embedded_parameters_result.has_value());
  assert(embedded_parameters_result->at(0).name == "web_search");
  assert(embedded_parameters_result->at(0).arguments["query"] == "latest world news");

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
  assert(embedded_type_result.has_value());
  assert(embedded_type_result->at(0).name == "web_search");
  assert(embedded_type_result->at(0).arguments["query"] == "latest news on artificial intelligence");

  auto embedded_parse = pbr::StructuredTextParser::ParseFromLlmOutput(embedded_tools);
  assert(!embedded_parse.ok);

  const std::string heading_clamp = R"({"blocks":[{"type":"heading","level":6,"text":"Big"}]})";
  auto heading_clamp_result = pbr::StructuredTextParser::ParseBlocksJson(heading_clamp);
  assert(!heading_clamp_result.ok);

  const std::string button_block = R"({
    "blocks": [
      { "type": "button", "label": "Say \"hi\"", "message": "It's a \\test" }
    ]
  })";
  auto button_result = pbr::StructuredTextParser::ParseBlocksJson(button_block);
  assert(button_result.ok);
  assert(button_result.rml.find("chat-suggestion") != std::string::npos);
  assert(button_result.rml.find("send_chat_action('__ENTRY__', 0)") != std::string::npos);
  assert(button_result.rml.find("send_suggestion(") == std::string::npos);
  assert(button_result.chat_actions.size() == 1);
  assert(button_result.chat_actions[0].label == "Say \"hi\"");
  assert(button_result.chat_actions[0].message == "It's a \\test");
  assert(!button_result.chat_actions[0].payload.has_value());

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
  assert(payload_button.ok);
  assert(payload_button.chat_actions.size() == 1);
  assert(payload_button.chat_actions[0].payload.has_value());
  assert(payload_button.chat_actions[0].payload->find("choice") != std::string::npos);

  const std::string multi_button = R"({
    "blocks": [
      { "type": "button", "label": "One", "message": "First" },
      { "type": "button", "label": "Two", "message": "Second" }
    ]
  })";
  auto multi_button_result = pbr::StructuredTextParser::ParseBlocksJson(multi_button);
  assert(multi_button_result.ok);
  assert(multi_button_result.chat_actions.size() == 2);
  assert(multi_button_result.rml.find("send_chat_action('__ENTRY__', 0)") != std::string::npos);
  assert(multi_button_result.rml.find("send_chat_action('__ENTRY__', 1)") != std::string::npos);

  const std::string button_missing_message = R"({"blocks":[{"type":"button","label":"Go"}]})";
  auto button_missing_message_result = pbr::StructuredTextParser::ParseBlocksJson(button_missing_message);
  assert(!button_missing_message_result.ok);

  const std::string button_empty_message = R"({"blocks":[{"type":"button","label":"Go","message":""}]})";
  auto button_empty_message_result = pbr::StructuredTextParser::ParseBlocksJson(button_empty_message);
  assert(!button_empty_message_result.ok);

  const std::string button_only_invalid = R"({"blocks":[{"type":"button","label":"Go"}]})";
  auto button_only_invalid_result = pbr::StructuredTextParser::ParseBlocksJson(button_only_invalid);
  assert(!button_only_invalid_result.ok);

  // LLMs often omit the final closing brace on the root object.
  const std::string missing_root_brace = R"({
    "blocks": [
      { "type": "paragraph", "text": "Hi" },
      { "type": "button", "label": "More", "message": "Tell me more" }
    ]
  )";
  auto repaired_result = pbr::StructuredTextParser::ParseBlocksJson(missing_root_brace);
  assert(repaired_result.ok);
  assert(repaired_result.rml.find("<p>Hi</p>") != std::string::npos);
  assert(repaired_result.rml.find("chat-suggestion") != std::string::npos);
  assert(repaired_result.rml.find("selectable=\"text\"") == std::string::npos);
  assert(repaired_result.chat_actions.size() == 1);
  assert(repaired_result.chat_actions[0].label == "More");
  assert(repaired_result.chat_actions[0].message == "Tell me more");

  const std::string form_block = R"({
    "blocks": [
      { "type": "form", "id": "booking", "title": "Book", "submit_label": "Go",
        "submit_template": "Book {{name}}",
        "fields": [{ "id": "name", "label": "Name", "field_type": "text" }] }
    ]
  })";
  auto form_result = pbr::StructuredTextParser::ParseBlocksJson(form_block);
  assert(form_result.ok);
  assert(form_result.widget_inits.size() == 1);
  assert(form_result.rml.find("data-value=\"field.value\"") != std::string::npos);
  assert(form_result.rml.find("submit_form('__ENTRY__', 'booking')") != std::string::npos);

  const std::string calendar_block = R"({
    "blocks": [
      { "type": "calendar", "month": 6, "year": 2026, "available_days": ["2026-06-15"] }
    ]
  })";
  auto calendar_result = pbr::StructuredTextParser::ParseBlocksJson(calendar_block);
  assert(calendar_result.ok);
  assert(calendar_result.widget_inits.size() == 1);
  assert(calendar_result.rml.find("calendar_prev('__ENTRY__')") != std::string::npos);
  assert(calendar_result.rml.find("turn.calendar.weeks") != std::string::npos);
  assert(calendar_result.rml.find("<table class=\"calendar-grid\">") != std::string::npos);

  const std::string calendar_defaults = R"({"blocks":[{"type":"calendar"}]})";
  auto calendar_defaults_result = pbr::StructuredTextParser::ParseBlocksJson(calendar_defaults);
  assert(calendar_defaults_result.ok);
  assert(calendar_defaults_result.widget_inits.size() == 1);

  const std::string card_block = R"({
    "blocks": [
      { "type": "card", "title": "Title", "body": "Body text" }
    ]
  })";
  auto card_result = pbr::StructuredTextParser::ParseBlocksJson(card_block);
  assert(card_result.ok);
  assert(card_result.rml.find("chat-card") != std::string::npos);

  const std::string poll_block = R"({
    "blocks": [
      { "type": "poll", "question": "Pick one", "options": [
        { "label": "A", "message": "Choose A" }
      ]}
    ]
  })";
  auto poll_result = pbr::StructuredTextParser::ParseBlocksJson(poll_block);
  assert(poll_result.ok);
  assert(poll_result.chat_actions.size() == 1);
  assert(poll_result.rml.find("chat-poll") != std::string::npos);

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
  auto long_list_result = pbr::StructuredTextParser::ParseBlocksJson(long_list_block);
  assert(long_list_result.ok);
  assert(long_list_result.rml.find("chat-long-list-scroll") != std::string::npos);
  assert(long_list_result.rml.find("chat-long-list-title") != std::string::npos);
  assert(long_list_result.rml.find("&lt;Q2&gt;") != std::string::npos);
  assert(long_list_result.rml.find("&amp;") != std::string::npos);
  assert(long_list_result.rml.find("chat-long-list-footer") != std::string::npos);
  assert(long_list_result.chat_actions.size() == 3);

  const std::string long_list_missing_items = R"({"blocks":[{"type":"long_list","title":"X"}]})";
  auto long_list_missing_items_result = pbr::StructuredTextParser::ParseBlocksJson(long_list_missing_items);
  assert(!long_list_missing_items_result.ok);

  std::cout << "structured_text_parser_test ok\n";
  return 0;
}
