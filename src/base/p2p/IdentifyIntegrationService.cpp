#include "base/p2p/IdentifyIntegrationService.h"

#include "base/p2p/Libp2pHost.h"
#include "base/p2p/PeerAddressBook.h"
#include "base/p2p/PeerSessionManager.h"

#include <libp2p/crypto/crypto_provider/crypto_provider_impl.hpp>
#include <libp2p/crypto/ecdsa_provider/ecdsa_provider_impl.hpp>
#include <libp2p/crypto/ed25519_provider/ed25519_provider_impl.hpp>
#include <libp2p/crypto/hmac_provider/hmac_provider_impl.hpp>
#include <libp2p/crypto/key_marshaller/key_marshaller_impl.hpp>
#include <libp2p/crypto/key_validator/key_validator_impl.hpp>
#include <libp2p/crypto/random_generator/boost_generator.hpp>
#include <libp2p/crypto/rsa_provider/rsa_provider_impl.hpp>
#include <libp2p/crypto/secp256k1_provider/secp256k1_provider_impl.hpp>
#include <libp2p/host/basic_host/basic_host.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/multi/multiaddress.hpp>
#include <libp2p/protocol/identify.hpp>
#include <libp2p/protocol/identify/config.hpp>

#include <span>

namespace pbr {
namespace {

std::shared_ptr<libp2p::crypto::marshaller::KeyMarshaller> CreateKeyMarshaller() {
  auto csprng = std::make_shared<libp2p::crypto::random::BoostRandomGenerator>();
  auto ed25519 = std::make_shared<libp2p::crypto::ed25519::Ed25519ProviderImpl>();
  auto rsa = std::make_shared<libp2p::crypto::rsa::RsaProviderImpl>();
  auto ecdsa = std::make_shared<libp2p::crypto::ecdsa::EcdsaProviderImpl>();
  auto secp256k1 =
      std::make_shared<libp2p::crypto::secp256k1::Secp256k1ProviderImpl>(csprng);
  auto hmac = std::make_shared<libp2p::crypto::hmac::HmacProviderImpl>();
  auto crypto_provider = std::make_shared<libp2p::crypto::CryptoProviderImpl>(
      csprng, ed25519, rsa, ecdsa, secp256k1, hmac);
  auto key_validator =
      std::make_shared<libp2p::crypto::validator::KeyValidatorImpl>(crypto_provider);
  return std::make_shared<libp2p::crypto::marshaller::KeyMarshallerImpl>(key_validator);
}

libp2p::host::BasicHost* AsBasicHost(libp2p::Host& host) {
  return dynamic_cast<libp2p::host::BasicHost*>(&host);
}

} // namespace

IdentifyIntegrationService::~IdentifyIntegrationService() {
  Stop();
}

Roe<void> IdentifyIntegrationService::Start(Libp2pHost& host, PeerSessionManager* sessions) {
  if (started_) {
    return {};
  }
  if (!host.IsRunning()) {
    return Error("libp2p host not running");
  }

  Roe<void> result = Error("identify start failed");
  host.PostAndWait([this, &host, sessions, &result]() {
    auto* basic = AsBasicHost(host.GetHost());
    if (!basic) {
      result = Error("host is not BasicHost");
      return;
    }
    if (!basic->getIdentityManager()) {
      result = Error("host identity manager unavailable");
      return;
    }

    auto key_marshaller = CreateKeyMarshaller();
    auto& cmgr = basic->getNetwork().getConnectionManager();
    msg_processor_ = std::make_shared<libp2p::protocol::IdentifyMessageProcessor>(
        *basic, cmgr, *basic->getIdentityManager(), key_marshaller);

    libp2p::protocol::IdentifyConfig config;
    identify_ = std::make_shared<libp2p::protocol::Identify>(config, *basic, msg_processor_,
                                                             basic->getBus());
    identify_push_ =
        std::make_shared<libp2p::protocol::IdentifyPush>(msg_processor_, basic->getBus());

    host_ = basic;
    sessions_ = sessions;

    if (sessions_) {
      (void)identify_->onIdentifyReceived([this](const libp2p::peer::PeerId& peer_id) {
        if (!sessions_) {
          return;
        }
        sessions_->NoteRemoteIdentify(peer_id.toBase58());
      });
    }

    identify_->start();
    identify_push_->start();
    started_ = true;
    result = {};
  });
  return result;
}

void IdentifyIntegrationService::Stop() {
  if (!started_ || !host_) {
    StopOnIo();
    return;
  }
  // Best-effort; host may already be stopping.
  StopOnIo();
}

void IdentifyIntegrationService::StopOnIo() {
  identify_push_.reset();
  identify_.reset();
  msg_processor_.reset();
  host_ = nullptr;
  sessions_ = nullptr;
  started_ = false;
}

Roe<void> IdentifyIntegrationService::PublishSelfAdvertisedAddrs(
    const std::vector<std::string>& multiaddrs) {
  if (!started_ || !host_ || !identify_push_) {
    return Error("identify not started");
  }
  if (multiaddrs.empty()) {
    return {};
  }

  std::vector<libp2p::multi::Multiaddress> parsed;
  parsed.reserve(multiaddrs.size());
  for (const std::string& ma : multiaddrs) {
    auto created = libp2p::multi::Multiaddress::create(ma);
    if (!created) {
      continue;
    }
    parsed.push_back(created.value());
  }
  if (parsed.empty()) {
    return Error("no valid advertised multiaddrs");
  }

  const libp2p::peer::PeerId self_id = host_->getId();
  auto& addr_repo = host_->getPeerRepository().getAddressRepository();
  (void)addr_repo.upsertAddresses(self_id, std::span<const libp2p::multi::Multiaddress>(parsed),
                                  libp2p::peer::ttl::kPermanent);

  if (sessions_) {
    for (const std::string& ma : multiaddrs) {
      (void)sessions_->UpsertBookEntry(self_id.toBase58(), ma, PeerAddrSource::Identify);
    }
  }

  identify_push_->pushUpdates();
  return {};
}

} // namespace pbr
