set(PP_BROWSER_BASE_SOURCES
  platform/AndroidAssetLocator.cpp
  platform/AndroidPathProvider.cpp
  platform/AppLifecycle.cpp
  platform/AssetIO.cpp
  platform/BrowserThread.cpp
  platform/DesktopAssetLocator.cpp
  platform/DesktopPathProvider.cpp
  platform/EnvCredentialStore.cpp
  platform/ExecutablePath.cpp
  platform/IAssetLocator.cpp
  platform/ICredentialStore.cpp
  platform/IosAssetLocator.cpp
  platform/IosPathProvider.cpp
  platform/IPathProvider.cpp
  platform/Platform.cpp
  platform/PlatformDefaults.cpp
  platform/PlatformEarlyInit.cpp
  platform/PlatformLogSink.cpp
  platform/PlatformNavigation.cpp
  platform/PlatformServices.cpp
  platform/SdlAppEvents.cpp
  platform/SdlAssetFileInterface.cpp
  net/HttpClient.cpp
  net/McpDirectoryClient.cpp
  net/McpInfraBridge.cpp
  net/McpRegistrationClient.cpp
  net/McpRelayClient.cpp
  net/ServiceClientFactory.cpp
  net/ServiceClientsImpl.cpp
  data/AppPaths.cpp
  data/Config.cpp
  data/ConfigJson.cpp
  data/LlmPreset.cpp
  data/ProfileRegistry.cpp
  data/SchemaVersion.cpp
  data/SessionStore.cpp
  data/UserPreferences.cpp
  people/ContactsStore.cpp
  people/Ed25519Signer.cpp
  people/IdentityStore.cpp
  messaging/AtAiParser.cpp
  messaging/ChatPayloadCodec.cpp
  messaging/DirectChatTarget.cpp
  messaging/RelayWirePayload.cpp
  messaging/JsonThreadStore.cpp
  messaging/MessagingJson.cpp
  messaging/PeopleDiscoveryBlocks.cpp
  messaging/SqliteThreadStore.cpp
  crypto/CryptoUtil.cpp
  crypto/PskFingerprint.cpp
  crypto/SessionKeyDeriver.cpp
  crypto/CanonicalAad.cpp
  crypto/MessageCipher.cpp
  crypto/EncryptedPayload.cpp
  crypto/ReplayWindow.cpp
  ai/LlmClient.cpp
  ai/PromptBuilder.cpp
  ai/RmlValidator.cpp
  ai/StructuredTextParser.cpp
  ai/ToolResultFormatter.cpp
  ai/TurnPlan.cpp
  ai/TurnTrace.cpp
  ai/WorkingSetPolicy.cpp
  ai/conversation/Conversation.cpp
  ai/conversation/SlidingWindowContextPolicy.cpp
  ai/conversation/ThreadContextPolicy.cpp
  ai/conversation/TurnCoordinator.cpp
  ai/conversation/UserMessageFormatter.cpp
  ai/mcp/McpClient.cpp
  ai/mcp/McpRuntime.cpp
  ai/mcp/SchemaAdapter.cpp
  ui/Theme.cpp
  ui/ViewCatalog.cpp
  ui/InputCoordinator.cpp
  ui/ContextMenuHost.cpp
  ${CMAKE_SOURCE_DIR}/src/libp2p/integration/host/Libp2pHost.cpp
)

add_library(pp_base STATIC ${PP_BROWSER_BASE_SOURCES})
target_include_directories(pp_base PUBLIC ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(pp_base PUBLIC
  pp_common
  pp_rmlui_backend
  RmlUi::Core
  ${PP_BROWSER_SDL3_TARGET}
  nlohmann_json::nlohmann_json
  CURL::libcurl
  SQLite::SQLite3
  sodium
)
if(TARGET OpenSSL::Crypto)
  target_link_libraries(pp_base PUBLIC OpenSSL::Crypto)
endif()

# Backward-compatible alias for tests and scripts that link pp_identity.
add_library(pp_identity ALIAS pp_base)
