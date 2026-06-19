#include "platform/ICredentialStore.h"

#include "platform/EnvCredentialStore.h"

namespace pbr {

namespace {
ICredentialStore* g_store = nullptr;
EnvCredentialStore g_env_default;
} // namespace

ICredentialStore& ICredentialStore::Instance() {
  return g_store ? *g_store : g_env_default;
}

void ICredentialStore::SetInstance(ICredentialStore* store) {
  g_store = store;
}

} // namespace pbr
