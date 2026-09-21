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

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
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

  void Pump();
  /** Mesh Tick only — Amp chat io_pump must not drain UI (Accept send would StartSfu early). */
  void PumpMesh();
  bool PumpUntil(const std::function<bool()>& done, int timeout_ms);

  /** Answerer: auto-Accept pending invite; exit when min RX frames met or hold expires. */
  int RunAnswererHold(int hold_seconds, int min_rx_frames);
  /** Offerer: StartCall → InCall → hold → Leave. */
  Roe<void> RunOffererCall(const std::string& peer_account, int hold_ms, int timeout_ms);

  void Shutdown();

private:
  ProductStackHarness() = default;
  Roe<void> InitStoresAndStack(const std::string& hop_ma);
  Roe<void> SendCallControl(const std::string& peer_account, const ThreadMessage& msg);
  void OnChatInbound(RelayEnvelope env);
  std::string AmpDialKeyForAccount(const std::string& account_id) const;
  void LearnAccountPeerId(const std::string& account_id, const std::string& peer_id);
  uint64_t RxAudioFrames() const;

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
  std::string local_account_;
  std::string local_peer_id_;
  std::string advertise_ma_;
  ByteVector shared_session_key_;
  /** Invite/Accept libp2p_peer_id → dial key (CallMediaHost map is private on CSM). */
  std::unordered_map<std::string, std::string> account_to_peer_id_;
};

} // namespace call_probe
} // namespace pbr
