#include "lib/messaging/ContactLocator.h"

#include "common/ValueJson.h"
#include "foundation/crypto/CryptoUtil.h"

#include <cctype>
#include <utility>

namespace pbr {

namespace {

constexpr size_t kMaxLocatorDevices = 16;

bool IsBase64UrlChar(const char c) {
  return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_';
}

} // namespace

std::string EncodeContactLocator(const ContactLocator& locator) {
  Object json;
  json.set("account_id", locator.account_id);
  if (!locator.nickname.empty()) {
    json.set("nickname", locator.nickname);
  }
  std::vector<Value> devices;
  for (const std::string& peer_id : locator.devices) {
    devices.emplace_back(peer_id);
  }
  json.set("devices", ArrayValue(std::move(devices)));
  const std::string text = DumpJson(json);
  return std::string(kContactLocatorPrefix) + Base64UrlEncode(ByteVector(text.begin(), text.end()));
}

Roe<ContactLocator> FindContactLocator(const std::string& text) {
  const size_t at = text.find(kContactLocatorPrefix);
  if (at == std::string::npos) {
    return Error("No contact locator");
  }
  size_t end = at + std::string(kContactLocatorPrefix).size();
  const size_t start = end;
  while (end < text.size() && IsBase64UrlChar(text[end])) {
    ++end;
  }
  auto bytes = Base64UrlDecode(text.substr(start, end - start));
  if (!bytes) {
    return Error("Contact locator does not decode");
  }
  auto json = TryParseObject(std::string(bytes.value().begin(), bytes.value().end()));
  if (!json) {
    return Error("Contact locator is not JSON");
  }
  ContactLocator locator;
  locator.account_id = json->getString("account_id").value_or("");
  if (locator.account_id.rfind("account:", 0) != 0) {
    return Error("Contact locator names no account");
  }
  locator.nickname = json->getString("nickname").value_or("");
  if (const Array* devices = json->getArray("devices")) {
    for (const Value& item : devices->elements) {
      if (auto peer_id = asString(item); peer_id && !peer_id->empty() && locator.devices.size() < kMaxLocatorDevices) {
        locator.devices.push_back(*peer_id);
      }
    }
  }
  return locator;
}

} // namespace pbr
