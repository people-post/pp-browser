#pragma once

#include "common/PbrCompat.h"
#include "foundation/diagnostics/CrashReportEnvelope.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace pbr {

/**
 * Channels whose log lines may leave the device in a diagnostics bundle. Allow-list, default deny: a line
 * from any other channel (including every channel not listed here) is dropped. Chat, AI and attachment
 * channels (ChatController, WebSearchTool, LlmClient, AgentSession, ChatThreadChrome, AttachmentCache) are
 * deliberately absent because their lines can carry user content.
 */
inline constexpr std::string_view kDiagnosticsChannels[] = {
    "Metrics", "Crash", "CallMediaBridge", "CallHopMigrateWorkflow", "CallTopologyController",
    "CallSessionWorkflow", "CallMediaLeg", "CallSessionManager", "Application", "ConversationsHub",
    "MeshDeliveryOrchestrator", "CallMediaSeat", "RelayReceivePipeline", "McpClient", "SettingsController",
    "LocalNotifier", "CallMediaConnect", "LiveCall", "CallMediaKeyExchange", "CallReachSignals", "pp-node",
    "CallController", "CallPresenter", "Broadcaster", "Runtime.Lifecycle", "DeferredStartup", "RmlMount",
    "CrashReport", "Runtime.AppRuntime", "ShellHost", "SseClient", "MediaDeviceArbiter", "CallMediaEngine",
    "CallHopPlanning", "CallMediaCoordinator", "CallStack", "BroadcastViewer", "startup", "BriefAiClient",
    "CallControlClient", "ProfileUnlockGate", "Config", "ThreadCompactionService", "PeerAccountBook",
    "ActionRouter", "AccountCardService", "Runtime.ThreadRuntime", "Runtime.OwnerThread", "BackgroundSync",
    "ClientCompat", "CallPathMobility", "HttpRelayClient", "ToolRegistry", "CallMediaPlane", "NetworkChange",
    "DeliveryOutbox", "AmpChatBlobTransport", "PeerTransports", "IosPaths", "Runtime.Coordinator",
    "ViewCatalog", "TurnTrace", "BroadcastParticipant", "CallsLoop", "CallInitiationBilling",
    "GroupMembershipWorkflow", "BroadcastServices", "PeerRouteBook", "AccountKeyResolver"};

inline constexpr std::size_t kDiagnosticsMaxJsonBytes = 2u * 1024u * 1024u;
inline constexpr int kDiagnosticsMaxLines = 30000;
inline constexpr std::size_t kDiagnosticsMaxLineBytes = 2048;

struct DiagnosticsBundleInput {
  std::vector<std::filesystem::path> log_files;  // newest first: pp-browser.log, pp-browser.1.log
  std::filesystem::path metrics_file;            // may not exist
  std::vector<std::string> crash_lines;          // already formatted "[ts] [INFO] [Crash] ..." lines
  std::vector<std::string> known_identifiers;    // own account id, contact peer ids, device id, relay id
  std::string client_token, product, version, os;
  int64_t captured_at_ms = 0;
};

struct DiagnosticsBundle {
  std::string json;
  int lines = 0;
  std::size_t bytes = 0;
};

/**
 * Filter (prefix, level >= INFO, channel allow-list) -> scrub -> cap -> self-check -> JSON.
 * On a self-check hit returns the error "self-check: <detector>" and builds nothing.
 */
Roe<DiagnosticsBundle> BuildDiagnosticsBundle(const DiagnosticsBundleInput& input);

/** Crash envelope as "[ts] [INFO] [Crash] key=value" lines: reason, frames, breadcrumb tags, sample. */
std::vector<std::string> CrashEnvelopeLines(const CrashReportEnvelope& envelope, std::string_view timestamp);

} // namespace pbr
