#include "domain/ai/ArticleFeedBlocks.h"

#include "common/PbrCompat.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>

namespace pbr {

namespace {

std::string Fill(std::string templ, const std::string& name, const std::string& value) {
  const std::string token = "{" + name + "}";
  for (size_t pos = templ.find(token); pos != std::string::npos; pos = templ.find(token, pos + value.size())) {
    templ.replace(pos, token.size(), value);
  }
  return templ;
}

/** Host of an http(s) URL for display; userinfo and port are dropped. Empty when there is none. */
std::string HostOf(const std::string& url) {
  const size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    return {};
  }
  const size_t start = scheme_end + 3;
  const size_t end = url.find_first_of("/?#", start);
  std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
  if (const size_t at = authority.rfind('@'); at != std::string::npos) {
    authority.erase(0, at + 1);
  }
  if (const size_t colon = authority.find(':'); colon != std::string::npos) {
    authority.erase(colon);
  }
  return authority;
}

bool IsHttpsUrl(const std::string& url) {
  return url.size() > 8 && url.compare(0, 8, "https://") == 0;
}

/** "MM-DD HH:MM" local time; the tool sends unix seconds or milliseconds. Empty when unusable. */
std::string TimeLabel(const int64_t created_at) {
  if (created_at <= 0) {
    return {};
  }
  const int64_t seconds = created_at > 100000000000LL ? created_at / 1000 : created_at;
  std::tm when{};
  if (!pp::civil_time::LocalTime(static_cast<std::time_t>(seconds), &when)) {
    return {};
  }
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%02d-%02d %02d:%02d", when.tm_mon + 1, when.tm_mday, when.tm_hour, when.tm_min);
  return buf;
}

Object MakeAction(const std::string& label, const std::string& message, Object payload, const std::string& style) {
  Object action;
  action.set("label", label);
  action.set("message", message);
  action.set("payload", ObjectValue(std::move(payload)));
  action.set("style", style);
  return action;
}

} // namespace

std::string UnwrapMcpTextResult(const std::string& raw) {
  const auto doc = TryParseObject(raw);
  if (!doc) {
    return raw;
  }
  const Array* content = doc->getArray("content");
  if (!content) {
    return raw;
  }
  for (const Value& part_value : content->elements) {
    const Object* part = asObject(part_value);
    if (!part || part->getString("type").value_or("") != "text") {
      continue;
    }
    if (auto text = part->getString("text"); text && !text->empty()) {
      return *text;
    }
  }
  return raw;
}

std::string BuildArticleFeedBlocksJson(const std::string& raw_json, const ArticleFeedBuildOptions& options) {
  auto parsed = ParseValue(UnwrapMcpTextResult(raw_json));
  if (!parsed) {
    return {};
  }
  const Object* doc = asObject(*parsed);
  const Array* articles = doc ? doc->getArray("articles") : nullptr;
  if (!articles) {
    return {};
  }
  const ArticleFeedLabels& labels = options.labels;

  std::vector<Value> blocks;
  Object paragraph;
  paragraph.set("type", "paragraph");

  std::vector<Value> items;
  std::string last_id;
  for (const Value& value : articles->elements) {
    if (items.size() >= options.max_items) {
      break;
    }
    const Object* article = asObject(value);
    if (!article) {
      continue;
    }
    const std::string title = article->getString("title").value_or("");
    const std::string content = article->getString("content").value_or("");
    if (title.empty() && content.empty()) {
      continue;
    }
    const std::string link = article->getString("link_to").value_or("");
    const std::string host = HostOf(link);
    const std::string time = TimeLabel(article->getIf<int64_t>("created_at").value_or(0));

    Object item;
    item.set("title", title.empty() ? content : title);
    if (!title.empty() && !content.empty()) {
      item.set("subtitle", content);
    }
    const std::string meta = host.empty() ? time : (time.empty() ? host : host + " \xC2\xB7 " + time);
    if (!meta.empty()) {
      item.set("meta", meta);
    }
    if (IsHttpsUrl(link)) {
      Object open_payload;
      open_payload.set("type", "open_url");
      open_payload.set("url", link);
      // "link": a small text link at the end of the article's text instead of a button under it.
      item.set("actions", ArrayValue({ObjectValue(MakeAction(labels.open, labels.open, std::move(open_payload),
                                                              "link"))}));
    }
    items.push_back(ObjectValue(std::move(item)));
    if (auto id = article->getString("id")) {
      last_id = *id;
    }
  }

  if (items.empty()) {
    paragraph.set("text", labels.empty);
    blocks.push_back(ObjectValue(std::move(paragraph)));
  } else {
    paragraph.set("text", Fill(labels.intro, "count", std::to_string(items.size())));
    blocks.push_back(ObjectValue(std::move(paragraph)));

    const size_t shown = items.size();
    Object list;
    list.set("type", "long_list");
    list.set("title", labels.title);
    list.set("items", ArrayValue(std::move(items)));

    // A full page means there may be older articles: the footer repeats the call with before_id.
    const size_t page_size = static_cast<size_t>(options.call_arguments.getNonNegInt("size").value_or(10));
    if (!last_id.empty() && shown >= page_size) {
      Object next = options.call_arguments;
      next.set("tool", options.tool_name);
      next.set("before_id", last_id);
      list.set("footer_actions",
               ArrayValue({ObjectValue(MakeAction(labels.more, labels.more_message, std::move(next), "secondary"))}));
    }
    blocks.push_back(ObjectValue(std::move(list)));
  }

  Object root;
  root.set("blocks", ArrayValue(std::move(blocks)));
  return DumpJson(root);
}

} // namespace pbr
