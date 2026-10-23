#include "common/directory/KeyCard.h"

#include <utility>

namespace pbr {

Object KeyCardToJson(const KeyCard& card) {
  Object json;
  json.set("account_id", card.account_id);
  json.set("signing_public_key_b64", card.signing_public_key_b64);
  json.set("kem_public_key_b64", card.kem_public_key_b64);
  json.set("kem_binding_signature_b64", card.kem_binding_signature_b64);
  return json;
}

Roe<KeyCard> KeyCardFromJson(const Object& json) {
  KeyCard card;
  for (auto [key, field] : {std::pair{"account_id", &card.account_id},
                            std::pair{"signing_public_key_b64", &card.signing_public_key_b64},
                            std::pair{"kem_public_key_b64", &card.kem_public_key_b64},
                            std::pair{"kem_binding_signature_b64", &card.kem_binding_signature_b64}}) {
    auto value = json.getString(key);
    if (!value || value->empty()) {
      return Error(std::string("key card: missing ") + key);
    }
    *field = std::move(*value);
  }
  return card;
}

} // namespace pbr
