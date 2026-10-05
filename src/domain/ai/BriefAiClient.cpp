#include "domain/ai/BriefAiClient.h"

#include "common/ValueJson.h"
#include "domain/ai/SseClient.h"
#include "foundation/crypto/CryptoUtil.h"
#include "foundation/error/AppError.h"

#include <algorithm>

namespace pbr {

namespace {

constexpr size_t kMaxHistoryTurns = 6;
constexpr size_t kMaxSources = 10;
constexpr std::string_view kHttpsPrefix = "https://";

std::string StringOrEmpty(const Object& json, const char* key) {
  return json.getString(key).value_or(std::string());
}

bool BearerAllowedFor(const std::string& url, const bool overridden) {
  if (!overridden || url.rfind("https://", 0) == 0) {
    return true;
  }
  return url.rfind("http://127.0.0.1", 0) == 0 || url.rfind("http://localhost", 0) == 0 ||
         url.rfind("http://[::1]", 0) == 0;
}

bool IsTerminal(BriefAiEvent::Type type) {
  return type == BriefAiEvent::Type::Done || type == BriefAiEvent::Type::Handoff ||
         type == BriefAiEvent::Type::Error;
}

} // namespace

BriefAiClient::BriefAiClient(LlmConfig config, std::string stream_url)
    : config_(std::move(config)), stream_url_(std::move(stream_url)) {
  redirectLogger("BriefAiClient");
}

Roe<void> BriefAiClient::ValidateImage(const BriefAiRequest& request) {
  if (!request.image) {
    return {};
  }
  const BriefAiImage& image = *request.image;
  if (image.mime != "image/png" && image.mime != "image/jpeg" && image.mime != "image/gif" &&
      image.mime != "image/webp") {
    return AppError::Config(Err::Config::Invalid, "unsupported image type: " + image.mime);
  }
  if (image.data.empty() || image.data.size() > kMaxImageBytes) {
    return AppError::Config(Err::Config::Invalid, "image must be 1 byte to 4 MB, got " +
                                                      std::to_string(image.data.size()) + " bytes");
  }
  return {};
}

std::string BriefAiClient::BuildRequestJson(const BriefAiRequest& request) {
  Object body;
  body.set("message", request.message);
  if (request.intent) {
    body.set("intent", *request.intent);
  }
  if (request.image) {
    Object image;
    image.set("mime", request.image->mime);
    image.set("data", Base64Encode(request.image->data));
    body.set("image", image);
  } else {
    body.set("image", Null{});
  }

  if (!request.history.empty() || !request.summary.empty()) {
    Object context;
    if (!request.history.empty()) {
      const size_t first = request.history.size() > kMaxHistoryTurns ? request.history.size() - kMaxHistoryTurns : 0;
      std::vector<Value> turns;
      for (size_t i = first; i < request.history.size(); ++i) {
        Object turn;
        turn.set("role", request.history[i].role);
        turn.set("content", request.history[i].content);
        turns.push_back(ObjectValue(std::move(turn)));
      }
      context.set("history", ArrayValue(std::move(turns)));
    }
    if (!request.summary.empty()) {
      context.set("summary", request.summary);
    }
    body.set("context", context);
  }

  Object client;
  client.set("app", "pp");
  if (!request.app_version.empty()) {
    client.set("version", request.app_version);
  }
  if (!request.platform.empty()) {
    client.set("platform", request.platform);
  }
  if (!request.lang.empty()) {
    client.set("lang", request.lang);
  }
  // Contract section 5: a request with an image is never handed off, so it carries no capabilities.
  if (!request.capabilities.empty() && !request.image) {
    std::vector<Value> capabilities;
    for (const std::string& capability : request.capabilities) {
      capabilities.emplace_back(capability);
    }
    client.set("capabilities", ArrayValue(std::move(capabilities)));
  }
  body.set("client", client);
  return DumpJson(body);
}

std::optional<BriefAiEvent> BriefAiClient::ParseEvent(const std::string& data) {
  auto json = TryParseObject(data); // "[DONE]" and garbage fail here
  if (!json) {
    return std::nullopt;
  }
  const std::string type = StringOrEmpty(*json, "type");
  BriefAiEvent event;
  if (type == "meta") {
    event.type = BriefAiEvent::Type::Meta;
    event.route = StringOrEmpty(*json, "route");
  } else if (type == "status") {
    event.type = BriefAiEvent::Type::Status;
    event.phase = StringOrEmpty(*json, "phase");
    event.tool = StringOrEmpty(*json, "tool");
    event.query = StringOrEmpty(*json, "query");
  } else if (type == "token") {
    event.type = BriefAiEvent::Type::Token;
    event.delta = StringOrEmpty(*json, "delta");
  } else if (type == "done") {
    event.type = BriefAiEvent::Type::Done;
    event.response = StringOrEmpty(*json, "response");
    event.route = StringOrEmpty(*json, "route");
    event.finish = StringOrEmpty(*json, "finish");
    if (const Array* sources = json->getArray("sources")) {
      for (const Value& value : sources->elements) {
        const Object* entry = asObject(value);
        if (!entry || event.sources.size() >= kMaxSources) {
          continue;
        }
        BriefAiSource source{StringOrEmpty(*entry, "title"), StringOrEmpty(*entry, "url"),
                             StringOrEmpty(*entry, "kind")};
        // The contract promises https only; be defensive since these become links.
        if (source.url.size() > kHttpsPrefix.size() && source.url.compare(0, kHttpsPrefix.size(), kHttpsPrefix) == 0) {
          event.sources.push_back(std::move(source));
        }
      }
    }
  } else if (type == "handoff") {
    event.type = BriefAiEvent::Type::Handoff;
    event.capability = StringOrEmpty(*json, "capability");
  } else if (type == "error") {
    event.type = BriefAiEvent::Type::Error;
    event.code = StringOrEmpty(*json, "code");
    event.message = StringOrEmpty(*json, "message");
    event.retryable = json->getIf<bool>("retryable").value_or(false);
  } else {
    return std::nullopt; // unknown type: ignored per the compatibility rule
  }
  return event;
}

Roe<BriefAiOutcome> BriefAiClient::Stream(const BriefAiRequest& request,
                                          const std::function<void(const BriefAiEvent&)>& on_event,
                                          const std::atomic<bool>& cancel) const {
  if (auto valid = ValidateImage(request); !valid) {
    return valid.error();
  }
  SseRequest sse;
  sse.url = stream_url_.empty() ? config_.base_url + std::string(kStreamPath) : stream_url_;
  // The user's key goes only to the derived gateway URL, or to an override that is https or loopback;
  // a stale dev override pointing at a plain-http host must not receive it.
  if (BearerAllowedFor(sse.url, !stream_url_.empty())) {
    sse.bearer_token = config_.api_key;
  }
  sse.json_body = BuildRequestJson(request);
  log().debug << "stream " << sse.url;

  std::optional<BriefAiOutcome> terminal;
  std::string route;
  size_t tokens = 0;
  size_t events = 0;
  auto handle = [&](const SseEvent& raw) {
    if (terminal) {
      return; // nothing counts after the terminal event
    }
    auto event = ParseEvent(raw.data);
    if (!event) {
      return;
    }
    ++events;
    if (event->type == BriefAiEvent::Type::Token) {
      ++tokens;
    }
    if (event->type == BriefAiEvent::Type::Meta || event->type == BriefAiEvent::Type::Done) {
      route = event->route;
    }
    if (IsTerminal(event->type)) {
      terminal = event->type == BriefAiEvent::Type::Done      ? BriefAiOutcome::Done
                 : event->type == BriefAiEvent::Type::Handoff ? BriefAiOutcome::Handoff
                                                              : BriefAiOutcome::Error;
    }
    on_event(*event);
  };

  auto result = SseClient().Post(sse, handle, cancel);
  if (!result) {
    return result.error();
  }
  if (*result == SseOutcome::Cancelled) {
    log().debug << "stream cancelled (" << events << " events, " << tokens << " tokens)";
    return BriefAiOutcome::Cancelled;
  }
  if (!terminal) {
    log().error << "stream ended without a terminal event (" << events << " events, " << tokens << " tokens)";
    return AppError::Network(Err::Network::HttpError, "answer interrupted");
  }
  log().debug << "stream ended outcome=" << static_cast<int>(*terminal) << " events=" << events
              << " tokens=" << tokens << " route=" << route;
  return *terminal;
}

} // namespace pbr
