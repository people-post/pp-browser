#pragma once

#include "domain/call_media/ICallMediaTransport.h"
#include "feature/calls/CallMediaInboundReply.h"
#include "feature/calls/CallSessionEvents.h"
#include "common/runtime/OwnerOutbox.h"
#include "common/Module.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "common/PbrCompat.h"

namespace pbr {

/** An inbound call-media hello the transport is asking us to accept. */
struct CallMediaInboundHello {
  std::string call_id;
  uint32_t media_epoch = 1;
  /** Dialer's mesh PeerId (the link it arrived on). */
  std::string peer_id;
};

/** What the owner decides about inbound bundles. All run on the calls owner. */
struct CallMediaInboundPorts {
  /** Session exists and still wants media; false → reject the hello. */
  std::function<bool(const std::string& call_id)> session_open;
  /** Epoch key for the hello, if already stored. */
  std::function<std::optional<ByteVector>(const std::string& call_id, uint32_t epoch)> load_key;
  /** Ask for the key to be fetched (e.g. inbox sync); called while waiting. */
  std::function<void(const std::string& call_id)> request_key;
  /** Bundle accepted (key found): the call's 1:1 path takes it and returns its callbacks. */
  std::function<CallMediaDirectCallbacks(const CallMediaInboundHello& hello)> on_accepted;
};

/**
 * The device's doorway for inbound 1:1 call media (V056, device level): it owns the transport's
 * inbound handler and hands each hello to the call it names. A hello is accepted once its epoch key
 * is available — the offerer often dials before the relay delivers the key — so it is parked on the
 * calls owner (bounded, asking for the key meanwhile) and answered when the key lands, the call's
 * session ends, the deadline passes or the doorway shuts down. No thread waits. Holds no call of its
 * own: a call id only names where a hello goes.
 *
 * Threading: passive, on the calls owner. Hellos and its timer come back through its outbox as
 * `DoorwayEvent`s, handled by `Handle`. `NotifyKeyAvailable()` is safe from any thread.
 */
class CallMediaDoorway : public Module {
public:
  explicit CallMediaDoorway(ICallMediaTransport& transport);
  ~CallMediaDoorway() override;
  CallMediaDoorway(const CallMediaDoorway&) = delete;
  CallMediaDoorway& operator=(const CallMediaDoorway&) = delete;

  /** Where its events go; installs the inbound handler once the ports are set too. */
  void SetOutbox(OwnerOutbox<DoorwayEvent> outbox);
  /** The inbound decisions; the handler is installed once the outbox is bound too. */
  void SetPorts(CallMediaInboundPorts ports);
  /** One of its own events, back on the calls owner. */
  void Handle(DoorwayEvent& event);
  /** A media key landed — wake hellos waiting for one. */
  void NotifyKeyAvailable();
  /** Release waiting hellos and drop the inbound handler (late hellos NACK at the transport). */
  void Shutdown();

  /** Inbound hello key wait budget (<= 0 = production default). */
  void SetKeyWaitMsForTest(int wait_ms);

private:
  /** A hello parked until its key lands (or the session ends / the deadline passes). */
  struct PendingHello {
    std::shared_ptr<CallMediaInboundReply> reply;
    std::chrono::steady_clock::time_point deadline;
  };

  void InstallHandler();
  void OnHello(std::shared_ptr<CallMediaInboundReply> hello);
  /** Fills params.media_key when the epoch key is stored. */
  bool TryLoadKey(CallMediaDirectConnectParams& params) const;
  void Accept(CallMediaInboundReply& reply);
  /** Answer each parked hello that can be answered now; `ask_again` re-requests missing keys. */
  void RecheckPending(bool ask_again);
  void ArmKeyPoll();
  /** NACK every parked hello (shutdown / destruction). */
  void RejectPending(const char* why);

  ICallMediaTransport& transport_;
  OwnerOutbox<DoorwayEvent> outbox_;
  CallMediaInboundPorts ports_;
  bool ports_set_ = false;
  bool installed_ = false;
  std::atomic<bool> shut_down_{false};
  std::vector<PendingHello> pending_;
  OwnerExecutor::TimerId key_poll_timer_id_ = 0;
  int key_wait_ms_;
};

} // namespace pbr
