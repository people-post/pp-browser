#include "domain/ai/SseClient.h"

#include "common/PlatformLimits.h"
#include "domain/ai/LlmClient.h"
#include "foundation/error/AppError.h"
#include "foundation/platform/CurlSsl.h"

#include <curl/curl.h>
#include "common/PbrCompat.h"

#include <algorithm>
#include <chrono>
#include <string_view>
#include <vector>

namespace pbr {

namespace {

struct StreamState {
  const std::function<void(const SseEvent&)>* on_event = nullptr;
  const std::atomic<bool>* cancel = nullptr;
  CURL* curl = nullptr;
  SseParser parser;
  std::string error_body; // body of an HTTP error response (not SSE)
  bool headers_checked = false;
  bool not_event_stream = false;
  bool handler_threw = false;
  long http_code = 0;
  size_t total_bytes = 0;
  size_t event_count = 0;
  std::chrono::steady_clock::time_point last_activity = std::chrono::steady_clock::now();
  bool cancelled = false;
  bool timed_out = false;
  bool limit_exceeded = false;
};

size_t WriteCallback(char* contents, size_t size, size_t nmemb, StreamState* state) {
  if (state->cancel->load()) {
    state->cancelled = true;
    return 0;
  }
  if (!state->headers_checked) {
    state->headers_checked = true;
    curl_easy_getinfo(state->curl, CURLINFO_RESPONSE_CODE, &state->http_code);
    // A 2xx that is not an event stream (a JSON completion, a gateway error page) must not pass
    // as an empty answer.
    char* content_type = nullptr;
    curl_easy_getinfo(state->curl, CURLINFO_CONTENT_TYPE, &content_type);
    static constexpr std::string_view kEventStream = "text/event-stream";
    const std::string_view type = std::string_view(content_type ? content_type : "").substr(0, kEventStream.size());
    state->not_event_stream =
        state->http_code < 400 &&
        !std::equal(kEventStream.begin(), kEventStream.end(), type.begin(), type.end(), [](char a, char b) {
          return a == ((b >= 'A' && b <= 'Z') ? static_cast<char>(b - 'A' + 'a') : b);
        });
  }
  state->last_activity = std::chrono::steady_clock::now();
  const size_t total = size * nmemb;
  if (total > kMaxLlmResponseBytes || state->total_bytes > kMaxLlmResponseBytes - total) {
    state->limit_exceeded = true;
    return 0;
  }
  state->total_bytes += total;

  if (state->http_code >= 400) {
    state->error_body.append(contents, total);
    return total;
  }
  if (state->not_event_stream) {
    return 0;
  }

  std::vector<SseEvent> events;
  if (!state->parser.Feed(std::string_view(contents, total), events)) {
    state->limit_exceeded = true;
    return 0;
  }
  // An exception must not unwind through libcurl.
  try {
    for (const SseEvent& event : events) {
      ++state->event_count;
      (*state->on_event)(event);
    }
  } catch (...) {
    state->handler_threw = true;
    return 0;
  }
  return total;
}

size_t HeaderCallback(char*, size_t size, size_t nitems, StreamState* state) {
  state->last_activity = std::chrono::steady_clock::now();
  return size * nitems;
}

int XferInfoCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
  auto* state = static_cast<StreamState*>(clientp);
  if (state->cancel->load()) {
    state->cancelled = true;
    return 1;
  }
  return 0;
}

} // namespace

SseClient::SseClient() {
  redirectLogger("SseClient");
}

Roe<SseOutcome> SseClient::Post(const SseRequest& request, const std::function<void(const SseEvent&)>& on_event,
                                const std::atomic<bool>& cancel) const {
  StreamState state;
  state.on_event = &on_event;
  state.cancel = &cancel;

  CURL* curl = curl_easy_init();
  if (!curl) {
    return AppError::Internal("curl init failed");
  }
  state.curl = curl;
  ApplyCurlSslDefaults(curl);

  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: text/event-stream");
  std::string bearer_header;
  if (!request.bearer_token.empty()) {
    bearer_header = "Authorization: Bearer " + request.bearer_token;
    headers = curl_slist_append(headers, bearer_header.c_str());
  }

  log().debug << "POST " << request.url;
  curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.json_body.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(request.json_body.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &state);
  curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
  curl_easy_setopt(curl, CURLOPT_HEADERDATA, &state);
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, XferInfoCallback);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &state);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, request.connect_timeout_s);

  // Drive the transfer through the multi interface so the cancel flag and the idle timeout are checked
  // every 50 ms even while the server is silent (the progress callback alone fires about once a second,
  // and CURLOPT_LOW_SPEED_* averages over ~5 s, so it noticed a 1 s idle only after ~7 s).
  CURLM* multi = curl_multi_init();
  curl_multi_add_handle(multi, curl);
  CURLcode code = CURLE_OK;
  bool multi_failed = false;
  int running = 1;
  while (running > 0) {
    if (cancel.load()) {
      state.cancelled = true;
      break;
    }
    if (std::chrono::steady_clock::now() - state.last_activity > std::chrono::seconds(request.idle_timeout_s)) {
      state.timed_out = true;
      break;
    }
    if (curl_multi_perform(multi, &running) != CURLM_OK) {
      multi_failed = true;
      break;
    }
    if (running > 0) {
      curl_multi_poll(multi, nullptr, 0, 50, nullptr);
    }
  }
  if (running == 0) {
    int pending = 0;
    while (CURLMsg* msg = curl_multi_info_read(multi, &pending)) {
      if (msg->msg == CURLMSG_DONE) {
        code = msg->data.result;
      }
    }
  }
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_multi_remove_handle(multi, curl);
  curl_multi_cleanup(multi);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  log().debug << "SSE stream ended (" << state.total_bytes << " bytes, " << state.event_count << " events)";

  if (state.cancelled) {
    return SseOutcome::Cancelled;
  }
  if (multi_failed) {
    return AppError::Internal("curl multi failed");
  }
  if (state.handler_threw) {
    return AppError::Internal("SSE event handler threw");
  }
  if (state.not_event_stream) {
    log().error << "HTTP " << http_code << " response is not an event stream";
    return AppError::Network(Err::Network::HttpError, "LLM response is not an event stream");
  }
  if (state.limit_exceeded) {
    return AppError::Network(Err::Network::HttpError,
                             "LLM response body exceeds limit of " + std::to_string(kMaxLlmResponseBytes) +
                                 " bytes");
  }
  if (http_code >= 400) {
    log().error << "HTTP " << http_code << " (" << state.error_body.size() << " bytes)";
    return LlmClient::MapHttpError(http_code, state.error_body);
  }
  if (state.timed_out || code == CURLE_OPERATION_TIMEDOUT) {
    log().error << (state.timed_out ? "SSE stream idle timeout" : "SSE connect timeout");
    return AppError::Network(Err::Network::Timeout, "SSE stream timed out");
  }
  if (code != CURLE_OK) {
    log().error << "curl failed: " << curl_easy_strerror(code);
    return AppError::Network(Err::Network::Unreachable, std::string("curl failed: ") + curl_easy_strerror(code));
  }
  return SseOutcome::Completed;
}

} // namespace pbr
