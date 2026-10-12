#pragma once

#include "foundation/data/Config.h"
#include "common/Error.h"

#include <string>
#include "common/PbrCompat.h"

namespace pbr {

/** The API keys a user typed into Settings. Kept out of config.json (PB-034); stored under the profile DEK. */
struct ConfigSecrets {
  std::string llm_api_key;
  std::string search_api_key;
  bool Empty() const { return llm_api_key.empty() && search_api_key.empty(); }
  bool operator==(const ConfigSecrets& other) const {
    return llm_api_key == other.llm_api_key && search_api_key == other.search_api_key;
  }
};

/** Where SessionStore keeps the typed keys. Ready() is false until the profile vault is unlocked; config.json
 *  never carries a key, so a typed key cannot be saved before that. */
class IConfigSecrets {
public:
  virtual ~IConfigSecrets() = default;
  virtual bool Ready() const = 0;
  virtual Roe<ConfigSecrets> Load() const = 0;
  virtual Roe<void> Store(const ConfigSecrets& secrets) = 0;
};

/** The typed keys in `config`: a key that came from an environment variable (api_key_env set) is not one. */
inline ConfigSecrets TypedSecretsOf(const AppConfig& config) {
  ConfigSecrets out;
  if (config.llm_api_key_env.empty()) {
    out.llm_api_key = config.llm.api_key;
  }
  if (config.search.api_key_env.empty()) {
    out.search_api_key = config.search.api_key;
  }
  return out;
}

/** Puts stored keys back into a config read from disk; env-derived keys are left alone. */
inline void OverlaySecrets(AppConfig& config, const ConfigSecrets& secrets) {
  if (config.llm_api_key_env.empty() && !secrets.llm_api_key.empty()) {
    config.llm.api_key = secrets.llm_api_key;
    config.llm.require_api_key = true;
  }
  if (config.search.api_key_env.empty() && !secrets.search_api_key.empty()) {
    config.search.api_key = secrets.search_api_key;
  }
}

} // namespace pbr
