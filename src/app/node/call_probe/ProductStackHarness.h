#pragma once

#include "amp/L1/Clock.h"
#include "amp/link/AmpStack.h"
#include "domain/mesh/host/MeshHost.h"
#include "domain/messaging/SqlitePskSessionStore.h"
#include "domain/messaging/SqliteThreadStore.h"
#include "domain/people/ContactsStore.h"
#include "domain/people/IdentityStore.h"
#include "feature/calls/CallControlInboundPorts.h"
#include "feature/calls/CallStack.h"
#include "feature/calls/CallUiBackend.h"
#include "feature/conversations/AmpDirectChatTransport.h"
#include "foundation/data/Config.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>

namespace pbr {
namespace call_probe {

/**
 * Hard-lab HL004 product-stack: real CallStack + CallUiBackend over MeshHost(AttachAmpStack).
 * GUI-click surface = StartCall / AcceptClicked / LeaveClicked; call-control rides Amp chat.
 */
class ProductStackHarness {
public:
  static Roe<std::unique_ptr<ProductStackHarness>> Create(std::unique_ptr<pp::amp::AmpStack> stack,
                                                          std::shared_ptr<pp::adp::Clock> clock,
                                                          std::string advertise_ma,
                                                          const std::string& hop_ma);

  ~ProductStackHarness();

  MeshHost& Host() { return *host_; }
  CallUiBackend& Ui() { return *ui_; }
  CallStack& Stack() { return *stack_; }
  const std::string& LocalAccountId() const { return local_account_; }
  const std::string& LocalPeerId() const { return local_peer_id_; }
  const std::string& AdvertiseMa() const { return advertise_ma_; }

  Roe<void> UpsertPeerContact(const std::string& account_id, const std::string& peer_id,
                              const std::string& multiaddr);
  Roe<void> EnsureOriginThread(const std::string& thread_id, const std::string& peer_account);
  /** Dual-SNAT: nested circuit to peer so Amp chat call-control can deliver before StartCall. */
  Roe<void> EnsurePeerCircuitPath(const std::string& peer_id);

  /**
   * Deliver call control through files under `dir` (a relay-inbox stand-in on the lab's shared
   * mount) instead of Amp chat. No peer link is built for signaling, so call media must reach
   * the peer from cold — the product shape (signaling via relay, media via mesh).
   */
  void SetSignalDir(std::filesystem::path dir);
  bool UsesSignalDir() const { return !signal_dir_.empty(); }
  /** Dirty dial book: register the peer's private advertise MA as dialable (dogfood / H010). */
  Roe<void> RegisterPeerPrivateEndpoint(const std::string& peer_id, const std::string& multiaddr);
  /**
   * Dirty dial book, worse: one EnsureAssociation to the (private, undialable) peer MA so the
   * link is left in dial backoff — the product reach must heal it. Backoff is NOT cleared here.
   */
  void ForceDialMiss(const std::string& peer_id);

  /** Run the UI mailbox (main thread = UI). The mesh runs on MeshHost's MeshPump. */
  void Pump();
  bool PumpUntil(const std::function<bool()>& done, int timeout_ms);
  /** LeaveClicked, then pump until Idle and the call_leave fanout has been sent. */
  void LeaveAndFlush(const std::string& call_id);
  /** Stderr marker for the shutdown step now running; the watchdog names it on a hang. */
  void ShutdownStep(const char* step);
  void ShutdownImpl();
  /**
   * Leave + teardown must finish within kTeardownBudget. A hang prints the step and aborts —
   * core in /share when the lab mounts it (gdb on the host needs no ptrace for a core).
   */
  void ArmTeardownWatchdog();
  void DisarmTeardownWatchdog();

  /** Answerer: auto-Accept pending invite; exit when min RX frames met or hold expires. */
  int RunAnswererHold(int hold_seconds, int min_rx_frames);
  /** Offerer: StartCall → InCall → hold → Leave. */
  Roe<void> RunOffererCall(const std::string& peer_account, int hold_ms, int timeout_ms);

  void Shutdown();

  /** Fail the hold when rx audio frames stop increasing for `ms` after media started (0 = off). */
  void SetRxStallMs(int ms) { rx_stall_ms_ = ms; }
  /** Answerer: judge stalls only for this long after the first rx frame (0 = whole hold). */
  void SetRxWatchMs(int ms) { rx_watch_ms_ = ms; }

private:
  ProductStackHarness() = default;
  Roe<void> InitStoresAndStack(const std::string& hop_ma);
  Roe<void> SendCallControl(const std::string& peer_account, const ThreadMessage& msg);
  Roe<void> WriteSignal(const std::string& peer_account, const RelayEnvelope& env);
  /** UI pump: hand files in our signal inbox to OnChatInbound on a worker (like relay IO). */
  void PollSignalInbox();
  std::filesystem::path SignalInbox(const std::string& account) const;
  void OnChatInbound(RelayEnvelope env);
  std::string AmpDialKeyForAccount(const std::string& account_id) const;
  void LearnAccountPeerId(const std::string& account_id, const std::string& peer_id);
  uint64_t RxAudioFrames() const;
  uint64_t TxAudioFrames() const;

  std::shared_ptr<pp::adp::Clock> clock_;
  std::unique_ptr<MeshHost> host_;
  std::filesystem::path data_dir_;
  std::unique_ptr<SqliteThreadStore> store_;
  std::unique_ptr<ContactsStore> contacts_;
  std::unique_ptr<IdentityStore> identity_;
  std::unique_ptr<SqlitePskSessionStore> psk_;
  AppConfig app_config_;
  std::unique_ptr<CallStack> stack_;
  std::unique_ptr<CallUiBackend> ui_;
  CallControlInboundPorts inbound_;
  std::unique_ptr<AmpDirectChatTransport> chat_;
  /** Call-control sends attempted — LeaveAndFlush waits on it (Leave fanout runs after Idle). */
  std::atomic<int> control_sends_{0};
  std::atomic<const char*> shutdown_step_{""};
  std::thread teardown_watchdog_;
  std::atomic<bool> teardown_done_{false};
  std::string local_account_;
  std::string local_peer_id_;
  std::string advertise_ma_;
  ByteVector shared_session_key_;
  /** Invite/Accept libp2p_peer_id → dial key (CallMediaHost map is private on CSM). */
  std::unordered_map<std::string, std::string> account_to_peer_id_;
  int rx_stall_ms_ = 0;
  int rx_watch_ms_ = 0;
  std::filesystem::path signal_dir_;
  std::chrono::steady_clock::time_point next_signal_poll_{};
  uint64_t signal_seq_ = 0;
};

} // namespace call_probe
} // namespace pbr
