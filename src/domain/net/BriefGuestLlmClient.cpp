#include "domain/net/BriefGuestLlmClient.h"

#include "foundation/error/AppError.h"
#include "domain/net/HttpClient.h"
#include "common/ValueJson.h"

namespace pbr {

namespace {

std::string TrimTrailingSlashes(std::string url) {
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

} // namespace

std::string ResolveBriefLlmApiKey(const std::string& registered_key, const std::string& guest_key) {
  if (!registered_key.empty()) {
    return registered_key;
  }
  return guest_key;
}

Roe<BriefGuestLlmMintResult> MintBriefGuestLlmKey(const std::string& llm_base_url,
                                                 const std::string& install_id) {
  const std::string base = TrimTrailingSlashes(llm_base_url);
  if (base.empty()) {
    return AppError::Internal("Brief LLM base URL is empty");
  }
  const std::string url = base + "/guest/start";

  Object body;
  if (!install_id.empty()) {
    body.set("install_id", install_id);
  }
  const std::string payload = DumpJson(body);

  auto response = HttpClient::Post(url, payload, {{"Content-Type", "application/json"}});
  if (!response) {
    return response.error();
  }
  if (response->status_code == 429) {
    return AppError::Network(Err::Network::HttpError, "Guest LLM mint rate limited")
        .WithUser("Brief free tier is temporarily rate-limited — try again later.");
  }
  if (response->status_code < 200 || response->status_code >= 300) {
    std::string detail = "Guest LLM mint failed (HTTP " + std::to_string(response->status_code) + ")";
    if (auto json = TryParseObject(response->body)) {
      if (const Object* err = json->getObject("error")) {
        if (auto message = err->getString("message")) {
          detail = *message;
        }
      }
    }
    return AppError::Network(Err::Network::HttpError, detail);
  }

  auto root = TryParseObject(response->body);
  if (!root) {
    return AppError::Network(Err::Network::HttpError, "Guest LLM mint response was not JSON");
  }
  auto key = root->getString("llm_api_key");
  if (!key || key->empty()) {
    return AppError::Network(Err::Network::HttpError, "Guest LLM mint missing llm_api_key");
  }
  BriefGuestLlmMintResult out;
  out.llm_api_key = *key;
  if (auto expires = root->getString("expires_at")) {
    out.expires_at = *expires;
  }
  return out;
}

} // namespace pbr
