#include "foundation/platform/BackendTable.h"

namespace pbr {

const char* BackendNetworkName(const BackendNetwork network) {
  switch (network) {
    case BackendNetwork::Staging:
      return "staging";
    case BackendNetwork::Dev:
      return "dev";
    case BackendNetwork::TestCustom:
      return "test-custom";
    case BackendNetwork::Main:
      break;
  }
  return "main";
}

bool CustomServices::operator==(const CustomServices& other) const {
  return org_relay_url == other.org_relay_url && org_relay_id == other.org_relay_id &&
         registration_url == other.registration_url && registration_relay_id == other.registration_relay_id &&
         directory_providers == other.directory_providers && account_locker_url == other.account_locker_url &&
         client_compat_url == other.client_compat_url && llm_gateway_url == other.llm_gateway_url &&
         llm_token_url == other.llm_token_url && promoted_mcp_url == other.promoted_mcp_url &&
         crash_intake_url == other.crash_intake_url && mesh_nodes_url == other.mesh_nodes_url &&
         seed_relays == other.seed_relays;
}

namespace {

BackendNetwork CustomNetwork(const BuildKind kind) {
  return kind == BuildKind::Tester ? BackendNetwork::TestCustom : BackendNetwork::Main;
}

/** Every service of one operator under one origin, the way the built-in backends and PP_BROWSER_ORIGIN have them. */
BackendTable UnderOneOrigin(const std::string& origin, const std::string& relay_id) {
  BackendTable table;
  if (origin.empty()) {
    return table;
  }
  const std::string relay = BriefRelayBaseUrlFor(origin);
  const std::string llm = BriefLlmBaseUrlFor(origin);
  table.org_relay_url = relay;
  table.org_relay_id = relay_id;
  table.registration_url = relay;
  table.registration_relay_id = relay_id;
  table.directory_providers = {relay};
  table.account_locker_url = relay;
  table.client_compat_url = relay;
  table.llm_gateway_url = llm;
  table.llm_token_url = llm;
  table.promoted_mcp_url = BriefMcpUrlFor(origin);
  table.crash_intake_url = relay;
  table.mesh_nodes_url = relay;
  return table;
}

BackendNetwork NetworkOf(const BackendEnv env) {
  switch (env) {
    case BackendEnv::Staging:
      return BackendNetwork::Staging;
    case BackendEnv::Dev:
      return BackendNetwork::Dev;
    case BackendEnv::Prod:
    case BackendEnv::Custom:
      break;
  }
  return BackendNetwork::Main;
}

} // namespace

namespace {

std::string NormalizedUrl(const std::string& url) {
  size_t begin = 0;
  size_t end = url.size();
  while (begin < end && (url[begin] == ' ' || url[begin] == '\t')) {
    ++begin;
  }
  while (end > begin && (url[end - 1] == ' ' || url[end - 1] == '\t' || url[end - 1] == '/')) {
    --end;
  }
  std::string out = url.substr(begin, end - begin);
  const size_t scheme_end = out.find("://");
  const size_t host_end = out.find('/', scheme_end == std::string::npos ? 0 : scheme_end + 3);
  for (size_t i = 0; i < (host_end == std::string::npos ? out.size() : host_end); ++i) {
    if (out[i] >= 'A' && out[i] <= 'Z') {
      out[i] = static_cast<char>(out[i] - 'A' + 'a');
    }
  }
  return out;
}

} // namespace

bool IsBuiltInPromotedMcpSeed(const std::string& url, const CompiledOrigins& compiled) {
  if (url.empty()) {
    return false;
  }
  const std::string wanted = NormalizedUrl(url);
  for (const char* origin : {compiled.prod, compiled.staging, compiled.dev}) {
    if (origin && *origin != '\0' && NormalizedUrl(BriefMcpUrlFor(NormalizedUrl(origin))) == wanted) {
      return true;
    }
  }
  return false;
}

BackendTable BuiltInBackendTable(const BackendEnv env, const CompiledOrigins& compiled) {
  if (env == BackendEnv::Custom) {
    return CustomBackendTable({}, compiled.kind);
  }
  const std::string origin = ResolveBriefOrigin(false, nullptr, env, compiled);
  const OrgRelayIdentity identity = ResolveOrgRelayIdentity(false, nullptr, env, compiled);
  BackendTable table = UnderOneOrigin(origin, identity.relay_id);
  const BackendEnv resolved = CompiledBackendFor(env, compiled);
  table.id = BackendEnvName(resolved);
  table.network = NetworkOf(resolved);
  table.org_relay_certificate_b64 = identity.certificate_b64;
  return table;
}

BackendTable OriginBackendTable(const std::string& origin, const std::string& relay_id, const BuildKind kind) {
  std::string trimmed = origin;
  while (!trimmed.empty() && trimmed.back() == '/') {
    trimmed.pop_back();
  }
  BackendTable table = UnderOneOrigin(trimmed, relay_id);
  table.id = kind == BuildKind::Tester ? "test-custom" : "custom";
  table.network = CustomNetwork(kind);
  return table;
}

BackendTable CustomBackendTable(const CustomServices& services, const BuildKind kind) {
  BackendTable table;
  table.id = kind == BuildKind::Tester ? "test-custom" : "custom";
  table.network = CustomNetwork(kind);
  table.org_relay_url = services.org_relay_url;
  table.org_relay_id = services.org_relay_id;
  table.registration_url = services.registration_url;
  table.registration_relay_id = services.registration_relay_id;
  table.directory_providers = services.directory_providers;
  table.account_locker_url = services.account_locker_url;
  table.client_compat_url = services.client_compat_url;
  table.llm_gateway_url = services.llm_gateway_url;
  table.llm_token_url = services.llm_token_url;
  table.promoted_mcp_url = services.promoted_mcp_url;
  table.crash_intake_url = services.crash_intake_url;
  table.mesh_nodes_url = services.mesh_nodes_url;
  table.seed_relays = services.seed_relays;
  return table;
}

BackendTable ResolveBackendTable(const bool ui_demo, const char* env_origin, const char* env_relay_id,
                                 const BackendEnv backend, const CompiledOrigins& compiled,
                                 const CustomServices& custom) {
  if (ui_demo) {
    BackendTable none;
    none.id = "prod"; // the demo app's profile key (BackendSwitchVisible)
    return none;
  }
  if (env_origin && *env_origin) {
    return OriginBackendTable(env_origin, env_relay_id ? env_relay_id : "", compiled.kind);
  }
  if (backend == BackendEnv::Custom) {
    return CustomBackendTable(custom, compiled.kind);
  }
  return BuiltInBackendTable(backend, compiled);
}

} // namespace pbr
