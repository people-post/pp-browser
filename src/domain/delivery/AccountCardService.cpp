#include "domain/delivery/AccountCardService.h"

#include "domain/peers/AccountCardPolicy.h"
#include "lib/messaging/PeerRequestAuth.h"

#include "common/Utilities.h"
#include "foundation/runtime/AppRuntime.h"

#include <algorithm>
#include <utility>

namespace pbr {

AccountCardService::AccountCardService(IdentityStore& identity, OwnCards& own, AccountKeyResolver& keys,
                                       ContactsStore& contacts)
    : identity_(identity), own_(own), keys_(keys), contacts_(contacts) {
  redirectLogger("AccountCardService");
}

void AccountCardService::Attach(AmpAccountCardTransport* transport) {
  {
    std::lock_guard lock(mutex_);
    transport_ = transport;
  }
  if (transport) {
    transport->SetAnswer([this](const AccountCardRequest& request, const std::string& remote_peer_id) {
      return Answer(request, remote_peer_id);
    });
  }
}

std::vector<std::string> AccountCardService::DevicesOf(const std::string& account_id) {
  auto kept = keys_.Kept(account_id);
  if (!kept || !*kept) {
    return {};
  }
  return (*kept)->devices;
}

AccountCardReply AccountCardService::Answer(const AccountCardRequest& request, const std::string& remote_peer_id) {
  auto identity = identity_.Get();
  if (!identity) {
    return AccountCardReply{.error = "not_available"};
  }
  CardServeContext context;
  context.own_account_id = identity->account_id;
  context.own_card = [this, account = identity->account_id]() {
    return own_.Card(DevicesOf(account), util::NowUnixMs());
  };
  context.own_keys = [this]() { return own_.Keys(); };
  context.authenticate = [this](const AccountCardRequest& asked, const std::string& remote) -> Roe<void> {
    if (auto verified = PeerRequestAuth::Verify(asked, remote, util::NowUnixMs(), keys_.KeptSigning()); !verified) {
      return verified.error();
    }
    // The asker proved its account on this link: its key card is worth keeping.
    if (asked.requester_keys) {
      (void)keys_.AddKeyCard(*asked.requester_keys, "device");
    }
    return {};
  };
  context.blocked = [this](const std::string& account_id) {
    auto blocked = contacts_.IsAccountBlocked(account_id);
    return blocked && *blocked;
  };
  context.audience_everyone = audience_everyone_;
  AccountCardReply reply = AnswerCardRequest(request, remote_peer_id, context);
  log().info << "card asked by " << remote_peer_id << ": " << (reply.card ? "card" : reply.key_card ? "key card" : reply.error);
  return reply;
}

void AccountCardService::Fetch(const std::string& peer_id, const std::string& account_id,
                               std::function<void(Roe<PeerCardRecord>)> on_done) {
  FetchReaching(peer_id, account_id, [on_done](Outcome, Roe<PeerCardRecord> result) {
    if (on_done) {
      on_done(std::move(result));
    }
  });
}

void AccountCardService::FetchReaching(const std::string& peer_id, const std::string& account_id,
                                       std::function<void(Outcome, Roe<PeerCardRecord>)> on_done) {
  // The device's own link first, then the account's dial key (a call registers that one).
  FetchOver({peer_id, account_id}, peer_id, account_id,
            [this, alive = std::weak_ptr<int>(alive_), peer_id, account_id,
             on_done](Outcome outcome, Roe<PeerCardRecord> result) {
              if (outcome != Outcome::NotReached || !reach_ || alive.expired()) {
                on_done(outcome, std::move(result));
                return;
              }
              // No link up: reach the device (it may be parked on a seed), then ask it once more.
              reach_(peer_id, [this, alive, peer_id, account_id, on_done, result](bool reached) mutable {
                if (!reached || alive.expired()) {
                  on_done(Outcome::NotReached, std::move(result));
                  return;
                }
                FetchOver({peer_id}, peer_id, account_id, on_done);
              });
            });
}

void AccountCardService::FetchOver(std::vector<std::string> link_keys, const std::string& peer_id,
                                   const std::string& account_id,
                                   std::function<void(Outcome, Roe<PeerCardRecord>)> on_done) {
  AmpAccountCardTransport* transport = nullptr;
  {
    std::lock_guard lock(mutex_);
    transport = transport_;
  }
  auto identity = identity_.Get();
  auto local_peer_id = own_.DevicePeerId();
  auto own_keys = own_.Keys();
  if (!transport || !identity || !local_peer_id || !own_keys || link_keys.empty()) {
    on_done(Outcome::NotReached, Error("account cards not available"));
    return;
  }
  AccountCardRequest request;
  request.account_id = account_id;
  request.requester_keys = *own_keys;
  if (auto signed_request =
          PeerRequestAuth::Sign(request, identity->account_id, *local_peer_id, util::NowUnixMs(),
                                [this](const std::vector<uint8_t>& bytes) { return identity_.SignBytes(bytes); });
      !signed_request) {
    on_done(Outcome::NotReached, signed_request.error());
    return;
  }
  const std::string link_key = link_keys.front();
  link_keys.erase(link_keys.begin());
  transport->FetchAsync(
      link_key, request,
      [this, link_key, link_keys = std::move(link_keys), peer_id, account_id, on_done](Roe<AccountCardReply> reply) mutable {
        if (!reply) {
          log().info << "card ask over " << link_key << " failed: " << reply.error().message;
          if (!link_keys.empty()) {
            FetchOver(std::move(link_keys), peer_id, account_id, std::move(on_done));
            return;
          }
          on_done(Outcome::NotReached, reply.error());
          return;
        }
        const int64_t now = util::NowUnixMs();
        auto card = AcceptCardReply(*reply, account_id, peer_id, now);
        if (!card) {
          on_done(Outcome::Answered, card.error());
          return;
        }
        auto kept = keys_.AddAccountCard(*card, now, "device");
        if (!kept) {
          on_done(Outcome::Answered, kept.error());
          return;
        }
        AddDevicesToContact(*kept);
        on_done(Outcome::Answered, std::move(kept.value()));
      });
}

void AccountCardService::NoteDevice(const std::string& account_id, const std::string& peer_id) {
  if (account_id.empty() || peer_id.empty()) {
    return;
  }
  const std::vector<std::string> known = DevicesOf(account_id);
  if (std::find(known.begin(), known.end(), peer_id) != known.end()) {
    return;
  }
  const int64_t now = util::NowUnixMs();
  {
    std::lock_guard lock(mutex_);
    Asking& asking = asking_[peer_id];
    if (now < asking.next_allowed_ms) {
      return;
    }
    asking.next_allowed_ms = now + kAskAgainAfterMs;  // until this ask settles
  }
  FetchReaching(peer_id, account_id,
            [this, alive = std::weak_ptr<int>(alive_), account_id, peer_id](Outcome outcome, Roe<PeerCardRecord> result) {
              if (alive.expired()) {
                return;
              }
              if (!result) {
                log().info << "card from " << peer_id << " not taken: " << result.error().message;
              }
              bool retry = false;
              {
                std::lock_guard lock(mutex_);
                Asking& asking = asking_[peer_id];
                const int64_t after = util::NowUnixMs();
                if (outcome == Outcome::NotReached && ++asking.tries < kMaxTries) {
                  asking.next_allowed_ms = after + kRetryAfterMs;
                  retry = true;
                } else {
                  asking.next_allowed_ms = after + kAskAgainAfterMs;
                  asking.tries = 0;
                }
              }
              if (retry) {
                // The link may still be coming up (a call names its device before media connects).
                AppRuntime::ScheduleCoordinatorOneShot(
                    std::chrono::milliseconds(kRetryAfterMs + 100),
                    [this, alive, account_id, peer_id]() {
                      if (!alive.expired()) {
                        NoteDevice(account_id, peer_id);
                      }
                    });
              }
            });
}

void AccountCardService::AddDevicesToContact(const PeerCardRecord& record) {
  auto found = contacts_.FindByIdentity(record.keys.account_id, mesh::ContactIdKind::Account);
  if (!found || !*found) {
    return;
  }
  Contact contact = **found;
  bool changed = false;
  for (const std::string& peer_id : record.devices) {
    const bool listed = std::any_of(contact.remote.endpoints.begin(), contact.remote.endpoints.end(),
                                    [&](const mesh::DirectoryEndpoint& row) { return row.peer_id == peer_id; });
    if (!listed) {
      contact.remote.endpoints.push_back(mesh::DirectoryEndpoint{.peer_id = peer_id});
      changed = true;
    }
  }
  if (changed) {
    SyncContactMirrors(contact);
    (void)contacts_.Upsert(contact);
  }
}

} // namespace pbr
