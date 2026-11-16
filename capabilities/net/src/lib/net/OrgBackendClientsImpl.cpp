#include "lib/net/OrgBackendClientsImpl.h"

#include "lib/net/RelayOrigins.h"

#include "common/Diag.h"
#include "common/Utilities.h"
#include "foundation/crypto/CryptoUtil.h"
#include "common/ValueJson.h"
#include "lib/net/HttpBlobClient.h"
#include "lib/net/HttpClient.h"
#include "lib/net/RelayApiSignPayload.h"
#include "foundation/error/AppError.h"
#include "common/directory/DirectoryJson.h"

#include "common/Logger.h"

namespace pbr {
namespace {
std::string EncodePathSegment(const std::string& value);
} // namespace
} // namespace pbr
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

HttpPushDeviceClient::HttpPushDeviceClient(std::string base_url, const bool public_only)
    : base_url_(std::move(base_url)), public_only_(public_only) {}

namespace {

/** A refused registry or device call: the audience's codes (MR007) typed, the rest by status as before. */
Error RegistryRefusal(IRelayAudience* audience, const std::string& action, const HttpResponse& response) {
  if (auto typed = NoteRelayRefusal(audience, response.status_code, response.body)) {
    return *typed;
  }
  const std::string code = RelayRefusalCode(response.body);
  if (response.status_code == 409 && (code == "registration_mismatch" || code == "binding_mismatch")) {
    return AppError::Auth(Err::Auth::ScopeMismatch, action + ": " + code);
  }
  return Error(action + " failed with status " + std::to_string(response.status_code));
}

} // namespace

Roe<std::string> HttpPushDeviceClient::PostDevice(const char* path, const PushDeviceRegistration& registration,
                                                  const char* last_key, const std::string& last,
                                                  const int64_t timestamp, const RelayCommandBytes& build) {
  if (base_url_.empty()) {
    return Error("Relay base_url not configured");
  }
  auto signed_call = SignRelayCommand(audience_.get(), auth_signer_, timestamp, build);
  if (!signed_call) {
    return signed_call.error();
  }
  Object body;
  body.set("account_id", registration.account_id);
  body.set("platform", registration.platform);
  body.set("device_id", registration.device_id);
  body.set(last_key, last);
  PutSignedRelayCommand(body, *signed_call);
  const auto response = HttpClient::Post(base_url_ + path, DumpJson(body), {{"Content-Type", "application/json"}},
                                         kMaxHttpClientBodyBytes, {}, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return RegistryRefusal(audience_.get(), "Device API", response.value());
  }
  return response.value().body;
}

Roe<void> HttpPushDeviceClient::RegisterDevice(const PushDeviceRegistration& registration) {
  const int64_t timestamp = util::NowUnixMs();
  auto answered = PostDevice("/v1/devices/register", registration, "push_token", registration.push_token, timestamp,
                             [&registration, timestamp](const std::string& audience) {
                               return BuildRelayApiDeviceRegisterSignBytes(
                                   audience, registration.account_id, registration.platform, registration.device_id,
                                   registration.push_token, timestamp);
                             });
  if (!answered) {
    return answered.error();
  }
  const auto root = TryParseObject(*answered);
  std::lock_guard lock(binding_mutex_);
  binding_ids_[registration.device_id] = root ? root->getString("binding_id").value_or("") : std::string();
  return {};
}

Roe<void> HttpPushDeviceClient::UnregisterOnce(const PushDeviceRegistration& registration,
                                               const std::string& binding_id) {
  const int64_t timestamp = util::NowUnixMs();
  auto answered = PostDevice("/v1/devices/unregister", registration, "binding_id", binding_id, timestamp,
                             [&registration, &binding_id, timestamp](const std::string& audience) {
                               return BuildRelayApiDeviceUnregisterSignBytes(
                                   audience, registration.account_id, registration.platform, registration.device_id,
                                   binding_id, timestamp);
                             });
  if (!answered) {
    return answered.error();
  }
  std::lock_guard lock(binding_mutex_);
  if (const auto it = binding_ids_.find(registration.device_id); it != binding_ids_.end() && it->second == binding_id) {
    binding_ids_.erase(it);
  }
  return {};
}

Roe<void> HttpPushDeviceClient::UnregisterDevice(const PushDeviceRegistration& registration) {
  std::string kept;
  {
    std::lock_guard lock(binding_mutex_);
    if (const auto it = binding_ids_.find(registration.device_id); it != binding_ids_.end()) {
      kept = it->second;
    }
  }
  if (!kept.empty()) {
    auto removed = UnregisterOnce(registration, kept);
    if (removed || AppError::CategoryOf(removed.error()) != ErrorCategory::Auth ||
        static_cast<Err::Auth>(removed.error().code) != Err::Auth::ScopeMismatch) {
      return removed;
    }
  }
  // No binding of ours known, or a newer one there: register (the relay answers the binding it holds now) and remove
  // that one, once.
  if (auto bound = RegisterDevice(registration); !bound) {
    return bound;
  }
  std::string fresh;
  {
    std::lock_guard lock(binding_mutex_);
    fresh = binding_ids_[registration.device_id];
  }
  if (fresh.empty()) {
    return Error("Device API: the relay answered no binding_id");
  }
  return UnregisterOnce(registration, fresh);
}

void DirectoryCardSink::Set(Keep keep) {
  std::lock_guard lock(mutex_);
  keep_ = std::move(keep);
}

void DirectoryCardSink::Give(const std::string& account_id, const Object& card) const {
  std::lock_guard lock(mutex_); // held while it keeps: clearing waits for a card being kept
  if (keep_) {
    keep_(account_id, card);
  }
}

HttpDirectoryClient::HttpDirectoryClient(std::string base_url, const HttpTimeout timeout,
                                         std::shared_ptr<const DirectoryCardSink> cards, const bool public_only)
    : base_url_(std::move(base_url)), timeout_(timeout), cards_(std::move(cards)), public_only_(public_only) {}

namespace {

template <typename Fn>
auto FailoverTry(const std::vector<std::unique_ptr<mesh::IDirectoryClient>>& backends, Fn&& fn)
    -> decltype(fn(*backends.front())) {
  using Result = decltype(fn(*backends.front()));
  if (backends.empty()) {
    return Error("Directory providers empty");
  }
  Result last_error = Error("Directory providers empty");
  for (const std::unique_ptr<mesh::IDirectoryClient>& backend : backends) {
    if (!backend) {
      continue;
    }
    auto result = fn(*backend);
    if (result) {
      return result;
    }
    last_error = result.error();
  }
  return last_error;
}

} // namespace

FailoverDirectoryClient::FailoverDirectoryClient(std::vector<std::unique_ptr<mesh::IDirectoryClient>> backends)
    : backends_(std::move(backends)) {}

Roe<std::vector<mesh::DirectoryHit>> FailoverDirectoryClient::SearchPeople(const std::string& query) {
  return FailoverTry(backends_, [&](mesh::IDirectoryClient& client) { return client.SearchPeople(query); });
}

Roe<mesh::DirectoryHit> FailoverDirectoryClient::LookupByAccount(const std::string& account_id) {
  return FailoverTry(backends_, [&](mesh::IDirectoryClient& client) { return client.LookupByAccount(account_id); });
}

Roe<std::vector<mesh::MeshNodeHit>> FailoverDirectoryClient::ListMeshNodes() {
  return FailoverTry(backends_, [](mesh::IDirectoryClient& client) { return client.ListMeshNodes(); });
}

Roe<std::vector<mesh::DirectoryHit>> HttpDirectoryClient::SearchPeople(const std::string& query) {
  if (base_url_.empty()) {
    return Error("Directory base_url not configured");
  }
  const std::string url = base_url_ + "/v1/search?q=" + EncodePathSegment(query); // free text from the user
  const auto response = HttpClient::Get(url, {}, kMaxHttpClientBodyBytes, timeout_, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Directory search failed with status " + std::to_string(response.value().status_code));
  }

  auto root = TryParseObject(response.value().body);
  const Array* hits_arr = root ? root->getArray("hits") : nullptr;
  if (!hits_arr) {
    return Error("Invalid directory search JSON");
  }

  std::vector<mesh::DirectoryHit> hits;
  for (const Value& item_value : hits_arr->elements) {
    const Object* item = asObject(item_value);
    if (!item) {
      continue;
    }
    hits.push_back(DirectoryHitFromJson(*item));
  }
  return hits;
}

namespace {

std::string EncodePathSegment(const std::string& value) {
  static constexpr char kHex[] = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size() * 3);
  for (const unsigned char c : value) {
    const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' ||
                            c == '_' || c == '.' || c == '~';
    if (unreserved) {
      out.push_back(static_cast<char>(c));
    } else {
      out.push_back('%');
      out.push_back(kHex[c >> 4]);
      out.push_back(kHex[c & 0x0F]);
    }
  }
  return out;
}

} // namespace

/** N032: what a mesh_node runs, at the top level of register/finish and of a /v1/mesh/nodes row. */
static void ApplyNodeClaims(Object& body, const RegistrationPublishOpts& publish) {
  if (publish.amp_wire > 0) {
    body.set("amp_wire", static_cast<int64_t>(publish.amp_wire));
  }
  if (!publish.node_version.empty()) {
    body.set("node_version", publish.node_version);
  }
  if (!publish.features.empty()) {
    std::vector<Value> features;
    for (const std::string& f : publish.features) {
      features.emplace_back(f);
    }
    body.set("features", makeArray(std::move(features)));
  }
}

void ApplyRegistrationPublishOpts(Object& body, const RegistrationPublishOpts& publish) {
  if (!publish.entity_kind.empty()) {
    body.set("entity_kind", publish.entity_kind);
  }
  if (publish.has_capabilities) {
    Object caps;
    caps.set("circuit_relay", publish.capabilities.circuit_relay);
    caps.set("media_relay", publish.capabilities.media_relay);
    caps.set("dht", publish.capabilities.dht);
    caps.set("ledger_gateway", publish.capabilities.ledger_gateway);
    body.set("capabilities", std::move(caps));
  }
  ApplyNodeClaims(body, publish);
}

Roe<mesh::DirectoryHit> HttpDirectoryClient::LookupByAccount(const std::string& account_id) {
  if (base_url_.empty()) {
    return Error("Directory base_url not configured");
  }
  if (account_id.rfind("account:", 0) != 0) {
    return Error("account_id must start with account:");
  }
  const std::string url = base_url_ + "/v1/users/by-account/" + EncodePathSegment(account_id);
  const auto response = HttpClient::Get(url, {}, kMaxHttpClientBodyBytes, timeout_, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code == 404) {
    return Error("Account not found"); // `not_registered`: not on this relay, or expired
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Directory lookup failed with status " + std::to_string(response.value().status_code));
  }

  auto root = TryParseObject(response.value().body);
  if (!root || root->getString("account_id").value_or("") != account_id) {
    return Error("Invalid account lookup JSON"); // the row must be the account asked for
  }
  if (const Object* card = root->getObject("account_card"); card && cards_) {
    cards_->Give(account_id, *card); // the account's own word on its relays and price, kept once it verifies
  }
  return DirectoryHitFromJson(*root);
}

Roe<std::optional<StoredAccountRow>> LookupStoredAccount(const std::string& relay_base_url,
                                                         const std::string& account_id, const bool public_only) {
  if (relay_base_url.empty() || account_id.rfind("account:", 0) != 0) {
    return Error("Stored account lookup: no relay or no Account ID");
  }
  if (public_only && !IsPublicRelayOrigin(relay_base_url)) {
    return Error("Stored account lookup: the relay is not on a public host");
  }
  const auto response = HttpClient::Get(relay_base_url + "/v1/users/by-account/" + EncodePathSegment(account_id), {},
                                        kMaxHttpClientBodyBytes, {}, public_only);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code == 404) {
    return std::optional<StoredAccountRow>{}; // not registered there
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Stored account lookup failed with status " + std::to_string(response.value().status_code));
  }
  auto root = TryParseObject(response.value().body);
  if (!root || root->getString("account_id").value_or("") != account_id) {
    return Error("Invalid account lookup JSON"); // the row must be the account asked for
  }
  StoredAccountRow row{.hit = DirectoryHitFromJson(*root)};
  if (const Object* card = root->getObject("account_card")) {
    row.card = *card;
  }
  return std::optional<StoredAccountRow>(std::move(row));
}

Roe<std::optional<Object>> FetchStoredAccountCard(const std::string& relay_base_url, const std::string& account_id,
                                                  const bool public_only) {
  auto row = LookupStoredAccount(relay_base_url, account_id, public_only);
  if (!row) {
    return row.error();
  }
  return *row && (*row)->card ? std::optional<Object>((*row)->card) : std::optional<Object>{};
}

Roe<std::vector<mesh::MeshNodeHit>> MeshNodeHitsFromDirectoryJson(const std::string& json) {
  auto root = TryParseObject(json);
  if (!root) {
    return Error("Invalid mesh nodes JSON");
  }
  const Array* nodes = root->getArray("nodes");
  if (!nodes) {
    return Error("Invalid mesh nodes JSON (missing nodes)");
  }
  std::vector<mesh::MeshNodeHit> out;
  out.reserve(nodes->elements.size());
  for (const Value& item : nodes->elements) {
    const Object* obj = asObject(item);
    if (!obj) {
      continue;
    }
    mesh::MeshNodeHit hit;
    if (auto account = obj->getString("account_id")) {
      hit.account_id = *account;
    }
    if (hit.account_id.empty()) {
      continue;
    }
    if (auto nick = obj->getString("nickname")) {
      hit.nickname = *nick;
    }
    if (auto expires = obj->getString("expires_at")) {
      hit.expires_at = *expires;
    }
    if (auto kind = obj->getString("entity_kind")) {
      hit.entity_kind = *kind;
    }
    if (hit.entity_kind.empty()) {
      hit.entity_kind = "mesh_node";
    }
    if (auto seq = obj->getIf<int64_t>("seq")) {
      hit.seq = *seq;
    }
    if (auto pk = obj->getString("signing_public_key_b64")) {
      hit.signing_public_key_b64 = *pk;
    }
    if (auto kem = obj->getString("kem_public_key_b64")) {
      hit.kem_public_key_b64 = *kem;
    }
    if (auto binding = obj->getString("kem_binding_signature_b64")) {
      hit.kem_binding_signature_b64 = *binding;
    }
    if (const Object* caps = obj->getObject("capabilities")) {
      if (auto circuit = caps->getIf<bool>("circuit_relay")) {
        hit.capabilities.circuit_relay = *circuit;
      }
      if (auto media = caps->getIf<bool>("media_relay")) {
        hit.capabilities.media_relay = *media;
      }
      if (auto dht = caps->getIf<bool>("dht")) {
        hit.capabilities.dht = *dht;
      }
      if (auto ledger = caps->getIf<bool>("ledger_gateway")) {
        hit.capabilities.ledger_gateway = *ledger;
      }
    }
    if (auto wire = obj->getIf<int64_t>("amp_wire"); wire && *wire > 0 && *wire < 16) {
      hit.amp_wire = static_cast<int>(*wire);
    }
    if (auto version = obj->getString("node_version")) {
      hit.node_version = *version;
    }
    if (const Array* features = obj->getArray("features")) {
      for (const Value& f : features->elements) {
        if (auto s = asString(f)) {
          hit.features.push_back(*s);
        }
      }
    }
    if (const Array* endpoints = obj->getArray("endpoints")) {
      for (const Value& ep_val : endpoints->elements) {
        const Object* ep = asObject(ep_val);
        if (!ep) {
          continue;
        }
        mesh::DirectoryEndpoint row;
        if (auto peer = ep->getString("peer_id")) {
          row.peer_id = *peer;
        }
        if (row.peer_id.empty()) {
          continue;
        }
        if (auto updated = ep->getIf<int64_t>("updated_at")) {
          row.updated_at = *updated;
        }
        if (const Array* mas = ep->getArray("multiaddrs")) {
          for (const Value& ma : mas->elements) {
            if (auto s = asString(ma)) {
              row.multiaddrs.push_back(*s);
            }
          }
        }
        hit.endpoints.push_back(std::move(row));
      }
    }
    out.push_back(std::move(hit));
  }
  return out;
}

Roe<std::vector<mesh::MeshNodeHit>> HttpDirectoryClient::ListMeshNodes() {
  if (base_url_.empty()) {
    return Error("Directory base_url not configured");
  }
  const auto response = HttpClient::Get(base_url_ + "/v1/mesh/nodes", {}, kMaxHttpClientBodyBytes, {}, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("Mesh nodes list failed with status " + std::to_string(response.value().status_code));
  }
  return MeshNodeHitsFromDirectoryJson(response.value().body);
}

Error RegistrationHttpStatusError(const std::string& step, const long status_code) {
  const bool refused = status_code >= 400 && status_code < 500 && status_code != 408 && status_code != 429;
  return AppError::Network(refused ? Err::Network::HttpRejected : Err::Network::HttpError,
                           "Registration " + step + " failed with status " + std::to_string(status_code));
}

HttpRegistrationClient::HttpRegistrationClient(std::string base_url, const bool public_only)
    : base_url_(std::move(base_url)), public_only_(public_only) {}

Roe<RegistrationStartResult> HttpRegistrationClient::StartRegistration(
    const std::string& public_key_b64, const std::string& nickname, const std::string& signature_alg,
    const std::string& kem_public_key_b64, const std::string& peer_id, const std::vector<std::string>& multiaddrs,
    const RegistrationPublishOpts& publish) {
  if (base_url_.empty()) {
    return Error("Registration base_url not configured");
  }
  Object body;
  body.set("public_key", public_key_b64);
  body.set("nickname", nickname);
  body.set("signature_alg", signature_alg);
  body.set("kem_public_key_b64", kem_public_key_b64);
  if (kem_public_key_b64.empty()) {
    return Error("kem_public_key_b64 is required");
  }
  if (!peer_id.empty()) {
    body.set("peer_id", peer_id);
  }
  if (!multiaddrs.empty()) {
    std::vector<Value> addrs;
    addrs.reserve(multiaddrs.size());
    for (const std::string& ma : multiaddrs) {
      addrs.push_back(Value(ma));
    }
    body.set("multiaddrs", ArrayValue(std::move(addrs)));
  }
  ApplyRegistrationPublishOpts(body, publish);
  const auto response =
      HttpClient::Post(base_url_ + "/v1/register/start", DumpJson(body), {{"Content-Type", "application/json"}},
                       kMaxHttpClientBodyBytes, {}, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return RegistrationHttpStatusError("start", response.value().status_code);
  }

  auto root = TryParseObject(response.value().body);
  auto challenge = root ? root->getString("challenge") : std::nullopt;
  if (!challenge) {
    return Error("Invalid registration start JSON");
  }

  RegistrationStartResult result;
  result.challenge = *challenge;
  if (auto alg = root->getString("signature_alg")) {
    result.signature_alg = *alg;
  } else {
    result.signature_alg = signature_alg;
  }
  if (auto expires = root->getString("expires_at")) {
    result.expires_at = *expires;
  }
  return result;
}

Roe<RegistrationResult> HttpRegistrationClient::FinishRegistration(
    const std::string& challenge, const std::string& public_key_b64, const std::string& nickname,
    const SignedRelayCommand& signed_finish, const std::string& signature_alg, const std::string& kem_public_key_b64,
    const std::string& kem_binding_signature_b64, const std::string& peer_id,
    const std::vector<std::string>& multiaddrs, const RegistrationPublishOpts& publish) {
  if (base_url_.empty()) {
    return Error("Registration base_url not configured");
  }
  Object body;
  body.set("challenge", challenge);
  body.set("public_key", public_key_b64);
  body.set("nickname", nickname);
  PutSignedRelayCommand(body, signed_finish);
  body.set("signature_alg", signature_alg);
  body.set("kem_public_key_b64", kem_public_key_b64);
  if (!kem_binding_signature_b64.empty()) {
    body.set("kem_binding_signature_b64", kem_binding_signature_b64);
  }
  if (kem_public_key_b64.empty()) {
    return Error("kem_public_key_b64 is required");
  }
  if (!peer_id.empty()) {
    body.set("peer_id", peer_id);
  }
  if (!multiaddrs.empty()) {
    std::vector<Value> addrs;
    addrs.reserve(multiaddrs.size());
    for (const std::string& ma : multiaddrs) {
      addrs.push_back(Value(ma));
    }
    body.set("multiaddrs", ArrayValue(std::move(addrs)));
  }
  ApplyRegistrationPublishOpts(body, publish);
  if (publish.account_card) {
    body.set("account_card", *publish.account_card);
  }
  const auto response =
      HttpClient::Post(base_url_ + "/v1/register/finish", DumpJson(body), {{"Content-Type", "application/json"}},
                       kMaxHttpClientBodyBytes, {}, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    // A refusal the relay names (wrong_audience: the pin is checked again) first; else refused or not by status.
    if (auto typed = NoteRelayRefusal(audience_.get(), response.value().status_code, response.value().body)) {
      return *typed;
    }
    return RegistrationHttpStatusError("finish", response.value().status_code);
  }

  auto root = TryParseObject(response.value().body);
  RegistrationResult result{.success = true};
  if (root) {
    if (auto success = root->getIf<bool>("success")) {
      result.success = *success;
    }
    if (auto account_id = root->getString("account_id")) {
      result.account_id = *account_id;
    }
    if (auto message = root->getString("message")) {
      result.message = *message;
    }
    if (auto llm_api_key = root->getString("llm_api_key");
        llm_api_key && takes_llm_key_.load(std::memory_order_relaxed)) {
      result.llm_api_key = *llm_api_key;
    }
    if (auto expires_at = root->getString("expires_at")) {
      result.expires_at = *expires_at;
    }
    result.registration_id = root->getString("registration_id").value_or("");
    result.account_card_status = root->getString("account_card_status").value_or("");
    result.kem_bound = root->getIf<bool>("kem_bound").value_or(false);
  }
  return result;
}

Roe<RegistrationResult> HttpRegistrationClient::UpdateNickname(const std::string& new_nickname,
                                                               const SignedRelayCommand& signed_update,
                                                               const std::string& account_id) {
  if (base_url_.empty()) {
    return Error("Registration base_url not configured");
  }
  Object body;
  body.set("account_id", account_id);
  body.set("nickname", new_nickname);
  PutSignedRelayCommand(body, signed_update);
  const auto response =
      HttpClient::Post(base_url_ + "/v1/profile/nickname", DumpJson(body), {{"Content-Type", "application/json"}},
                       kMaxHttpClientBodyBytes, {}, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return RegistryRefusal(audience_.get(), "Nickname update", response.value());
  }
  return RegistrationResult{.success = true, .message = "Nickname updated"};
}

Roe<void> HttpRegistrationClient::RevokeRegistration(const std::string& account_id, const std::string& registration_id,
                                                     const SignedRelayCommand& signed_revoke) {
  if (base_url_.empty()) {
    return Error("Registration base_url not configured");
  }
  Object body;
  body.set("account_id", account_id);
  body.set("registration_id", registration_id);
  PutSignedRelayCommand(body, signed_revoke);
  // Called on the way into a profile reset: keep a stuck network from holding the UI for long.
  const auto response =
      HttpClient::Post(base_url_ + "/v1/register/revoke", DumpJson(body), {{"Content-Type", "application/json"}},
                       kMaxHttpClientBodyBytes, HttpTimeout{.total_s = 10, .connect_s = 5}, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return RegistryRefusal(audience_.get(), "Registration revoke", response.value());
  }
  return {};
}

HttpClientCompatClient::HttpClientCompatClient(std::string base_url, const bool public_only)
    : base_url_(std::move(base_url)), public_only_(public_only) {}

Roe<ClientCompatDocument> HttpClientCompatClient::Fetch() {
  if (base_url_.empty()) {
    return Error("client-compat base_url not configured");
  }
  const auto response = HttpClient::Get(base_url_ + "/v1/client-compat", {}, kMaxHttpClientBodyBytes, {}, public_only_);
  if (!response) {
    return response.error();
  }
  if (response.value().status_code < 200 || response.value().status_code >= 300) {
    return Error("client-compat fetch failed with status " + std::to_string(response.value().status_code));
  }
  return ParseClientCompatDocument(response.value().body);
}

} // namespace pbr
