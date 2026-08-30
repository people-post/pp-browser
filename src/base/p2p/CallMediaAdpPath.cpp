#include "base/p2p/CallMediaAdpPath.h"

#include "base/adp/Clock.h"
#include "base/adp/OsUdpDatagramIo.h"
#include "base/p2p/CallMediaFrameCrypto.h"
#include "base/p2p/ReachabilityNetIf.h"

#include <cstdio>
#include <cstring>

namespace pbr {

bool ParseIpv4Dotted(const std::string& s, uint8_t out[4]) {
  unsigned a = 0, b = 0, c = 0, d = 0;
  if (std::sscanf(s.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
    return false;
  }
  if (a > 255 || b > 255 || c > 255 || d > 255) {
    return false;
  }
  out[0] = static_cast<uint8_t>(a);
  out[1] = static_cast<uint8_t>(b);
  out[2] = static_cast<uint8_t>(c);
  out[3] = static_cast<uint8_t>(d);
  return true;
}

std::string GuessPrimaryIpv4() {
  // Prefer an up, non-loopback LAN IPv4 from the shared netif helpers (Posix/Win32 split).
  for (const auto& nif : reachability_netif::LanIpv4Interfaces()) {
    if (nif.up_running_non_loopback && nif.ip.rfind("127.", 0) != 0) {
      return nif.ip;
    }
  }
  for (const auto& nif : reachability_netif::LanIpv4Interfaces()) {
    if (nif.ip.rfind("127.", 0) != 0) {
      return nif.ip;
    }
  }
  return "127.0.0.1";
}

CallMediaAdpPath::~CallMediaAdpPath() { Stop(); }

void CallMediaAdpPath::SetIoForTest(std::shared_ptr<adp::DatagramIo> io,
                                    std::shared_ptr<adp::Clock> clock) {
  std::lock_guard lock(mu_);
  io_ = std::move(io);
  clock_ = std::move(clock);
  test_io_ = true;
  endpoint_.reset();
  conn_.reset();
  active_ = false;
}

Roe<void> CallMediaAdpPath::EnsureEndpoint() {
  if (endpoint_) {
    return {};
  }
  if (!io_ || !clock_) {
    return Error("adp path: missing io/clock");
  }
  endpoint_ = std::make_unique<adp::Endpoint>(io_, clock_);
  return {};
}

Roe<CallMediaAdpHelloOffer> CallMediaAdpPath::BindLocal(const bool offerer_mints_assoc) {
  std::lock_guard lock(mu_);
  if (endpoint_ && local_.port != 0) {
    if (offerer_mints_assoc) {
      bool zero = true;
      for (uint8_t b : local_.assoc.bytes) {
        if (b != 0) {
          zero = false;
          break;
        }
      }
      if (zero) {
        local_.assoc = MintCallMediaAdpAssocId();
      }
    }
    return local_;
  }
  if (!test_io_) {
    auto bound = adp::OsUdpDatagramIo::Bind(adp::IpEndpoint::V4(0, 0, 0, 0, 0));
    if (!bound) {
      return bound.error();
    }
    io_ = std::shared_ptr<adp::DatagramIo>(std::move(*bound));
    clock_ = std::make_shared<adp::WallClock>();
  }
  if (auto err = EnsureEndpoint(); !err) {
    return err.error();
  }
  local_.port = io_->LocalEndpoint().port;
  if (test_io_) {
    const auto& ep = io_->LocalEndpoint();
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ep.addr[0], ep.addr[1], ep.addr[2], ep.addr[3]);
    local_.ipv4 = buf;
  } else {
    local_.ipv4 = GuessPrimaryIpv4();
  }
  if (offerer_mints_assoc) {
    local_.assoc = MintCallMediaAdpAssocId();
  }
  return local_;
}

CallMediaAdpHelloOffer CallMediaAdpPath::LocalOffer() const {
  std::lock_guard lock(mu_);
  return local_;
}

void CallMediaAdpPath::SetLocalAssoc(const adp::AssocId& id) {
  std::lock_guard lock(mu_);
  local_.assoc = id;
}

Roe<void> CallMediaAdpPath::Activate(const ByteVector& media_key, const std::string& call_id,
                                     const uint32_t media_epoch, const CallMediaAdpHelloOffer& remote,
                                     MediaHandler on_media) {
  std::lock_guard lock(mu_);
  if (!remote.valid()) {
    return Error("adp path: remote offer incomplete");
  }
  if (!endpoint_ && !test_io_) {
    return Error("adp path: BindLocal first");
  }
  if (!endpoint_) {
    if (auto err = EnsureEndpoint(); !err) {
      return err.error();
    }
  }
  auto key = DeriveCallMediaAdpAssocKey(media_key, call_id, media_epoch);
  if (!key) {
    return key.error();
  }
  uint8_t ip[4] = {};
  if (!ParseIpv4Dotted(remote.ipv4, ip)) {
    return Error("adp path: bad remote ip");
  }
  // Answerer may not have minted; adopt remote assoc when local is unset.
  adp::AssocId assoc = local_.assoc;
  bool local_zero = true;
  for (uint8_t b : local_.assoc.bytes) {
    if (b != 0) {
      local_zero = false;
      break;
    }
  }
  if (local_zero) {
    assoc = remote.assoc;
    local_.assoc = assoc;
  }

  adp::OpenParams op;
  op.key = *key;
  op.id = assoc;
  op.mint_id = false;
  op.peer = adp::IpEndpoint::V4(ip[0], ip[1], ip[2], ip[3], remote.port);

  // Re-open clean connection for this call.
  if (conn_) {
    conn_->Close();
    conn_.reset();
  }
  auto opened = endpoint_->Open(op);
  if (!opened) {
    return opened.error();
  }
  conn_ = *opened;
  media_key_ = media_key;
  call_id_ = call_id;
  media_epoch_ = media_epoch;
  on_media_ = std::move(on_media);
  active_ = true;

  auto self_conn = conn_;
  self_conn->OnMessage([this](const adp::Message& msg) {
    MediaHandler handler;
    ByteVector key;
    std::string cid;
    uint32_t epoch = 1;
    {
      std::lock_guard lock(mu_);
      if (!active_ || !on_media_) {
        return;
      }
      handler = on_media_;
      key = media_key_;
      cid = call_id_;
      epoch = media_epoch_;
    }
    auto decoded = DecryptCallMediaFrame(key, cid, epoch, msg.payload);
    if (!decoded) {
      return;
    }
    if (decoded->channel != kCallMediaChannelAudio) {
      return;
    }
    handler(decoded->channel, decoded->payload);
  });
  return {};
}

void CallMediaAdpPath::Stop() {
  std::lock_guard lock(mu_);
  active_ = false;
  on_media_ = nullptr;
  if (conn_) {
    conn_->Close();
    conn_.reset();
  }
  endpoint_.reset();
  if (!test_io_) {
    io_.reset();
    clock_.reset();
  }
  local_ = {};
}

bool CallMediaAdpPath::IsActive() const {
  std::lock_guard lock(mu_);
  return active_ && conn_ && !conn_->IsClosed();
}

bool CallMediaAdpPath::LooksAlive() const {
  std::lock_guard lock(mu_);
  if (!active_ || !conn_ || !clock_) {
    return false;
  }
  return conn_->LooksAlive(clock_->NowMs());
}

Roe<void> CallMediaAdpPath::SendOpus(const std::vector<uint8_t>& opus_payload, const uint32_t seq,
                                     const uint8_t mark) {
  std::shared_ptr<adp::Connection> conn;
  ByteVector key;
  std::string cid;
  uint32_t epoch = 1;
  {
    std::lock_guard lock(mu_);
    if (!active_ || !conn_ || conn_->IsClosed()) {
      return Error("adp path: inactive");
    }
    conn = conn_;
    key = media_key_;
    cid = call_id_;
    epoch = media_epoch_;
  }
  auto body =
      EncryptCallMediaFrame(key, cid, epoch, seq, mark, kCallMediaChannelAudio, opus_payload);
  if (!body) {
    return body.error();
  }
  if (body->size() > adp::kMaxPayload) {
    return Error("adp path: opus ciphertext too large");
  }
  auto err = conn->Send(adp::QosClass::BestEffort, *body);
  Pump();
  return err;
}

void CallMediaAdpPath::Pump() {
  adp::Endpoint* raw = nullptr;
  {
    std::lock_guard lock(mu_);
    raw = endpoint_.get();
  }
  if (raw) {
    raw->Pump();
    raw->Tick();
  }
}

} // namespace pbr
