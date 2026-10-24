#pragma once

#include "lib/messaging/AccountCardRequest.h"
#include "mesh/host/MeshPorts.h"

#include "common/Error.h"

#include <chrono>
#include <functional>
#include <memory>
#include <string>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

/**
 * `/pp/account-card/1.0.0` (docs/contracts/ACCOUNT_CARD.md#card-exchange-over-amp): one get_card request,
 * one reply, per channel. Serves this account's card through `Answer` (the policy is the caller's: who
 * may have it), and asks another device for its account's card. Codecs and checks stay Amp-free.
 */
class AmpAccountCardTransport {
public:
  using IoPump = std::function<void()>;
  using WorkerPost = std::function<void(std::function<void()>)>;
  using IoPost = std::function<void(std::function<void()>)>;
  using IoAfter = std::function<void(std::chrono::milliseconds, std::function<void()>)>;
  /** What to answer `request` from the device `remote_peer_id` (the link's authenticated PeerId). Worker thread. */
  using Answer = std::function<AccountCardReply(const AccountCardRequest& request, const std::string& remote_peer_id)>;

  AmpAccountCardTransport(mesh::IChatPeerLinks& links, IoPump io_pump, WorkerPost post_worker, IoPost post_io = {},
                          IoAfter post_after = {});
  ~AmpAccountCardTransport();

  AmpAccountCardTransport(const AmpAccountCardTransport&) = delete;
  AmpAccountCardTransport& operator=(const AmpAccountCardTransport&) = delete;

  void Start();
  void Stop();
  void SetAnswer(Answer answer);

  /** Ask device `peer_id` for a card; `on_done` once, on an I/O or worker thread. */
  void FetchAsync(const std::string& peer_id, const AccountCardRequest& request,
                  std::function<void(Roe<AccountCardReply>)> on_done);

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  mesh::IChatPeerLinks& links_;
  IoPump io_pump_;
  IoPost post_io_;
  IoAfter post_after_;
  bool started_ = false;
};

} // namespace pbr
