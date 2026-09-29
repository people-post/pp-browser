#pragma once

// Shared fakes for feature/broadcast gtests: a dial registry that reaches everything and a
// scripted media_relay client (attach per hop, sessions ending on demand, frames recorded).

#include "domain/mesh/l4/media_relay/client/IMediaRelayClient.h"
#include "domain/mesh/reach/MeshReachPorts.h"

#include <opus.h>

#include <atomic>
#include <functional>
#include <mutex>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pbr::test {

inline constexpr const char* kFakeRelaySelf = "12D3KooWViewer";

class FakeDial final : public IDialRegistry {
public:
  Roe<void> RegisterEndpoint(const std::string& /*peer_key*/, const std::string& /*multiaddr*/) override { return {}; }
  bool IsDialable(const std::string& /*peer_key*/) const override { return true; }
  std::optional<std::string> PreferredMultiaddr(const std::string& /*peer_key*/) const override { return std::nullopt; }
  void ClearDialBackoff(const std::string& /*peer_key*/) override {}
  void AbortInflightDial(const std::string& /*peer_key*/) override {}
  void ClearPeerCircuitHop(const std::string& /*peer_key*/) override {}
};

class FakeRelay final : public IMediaRelayClient {
public:
  Roe<std::string> LocalPeerIdBase58() const override { return std::string(kFakeRelaySelf); }
  bool IsStarted() const override { return true; }
  Roe<MediaRelayQuote> RequestQuote(const std::string& /*hop*/, const MediaRelayQuoteRequest& request,
                                    int /*timeout_ms*/) override {
    last_quote = request;
    MediaRelayQuote q;
    q.ok = true;
    q.quote_id = "q";
    q.rate = rate;
    return q;
  }
  Roe<MediaRelayAttachResult> AcceptAndAttach(const std::string&, const std::string&, const std::string&,
                                              const std::string&, std::function<void(MediaDataFrame)>,
                                              int) override {
    return Error("sync unused");
  }
  void AcceptAndAttachAsync(const std::string& hop, const std::string& /*quote_id*/, const std::string& session_id,
                            const std::string& /*auth*/, std::function<void(MediaDataFrame)> on_frame,
                            std::function<void(Roe<MediaRelayAttachResult>)> on_done, int /*timeout_ms*/) override {
    attach_hops.push_back(hop);
    last_session = session_id;
    auto complete = [this, hop, on_frame = std::move(on_frame), on_done = std::move(on_done)]() mutable {
      MediaRelayAttachResult r;
      r.ok = failing_hops.count(hop) == 0;
      r.error = r.ok ? "" : "hop refused attach";
      if (r.ok) {
        attached = true;
        attached_hop = hop;
        sink = std::move(on_frame);
      }
      on_done(r);
    };
    if (hold_attach) {
      held = std::move(complete);
    } else {
      complete();
    }
  }
  void StartClientFrameReader() override { ++reader_starts; }
  uint64_t AddClientTransportLostObserver(std::function<void(MediaRelayClientLoss)> observer) override {
    observers[next_token] = std::move(observer);
    return next_token++;
  }
  void RemoveClientTransportLostObserver(uint64_t token) override { observers.erase(token); }
  Roe<MediaRelayAttachResult> AttachAsLocalHop(const std::string&, std::function<void(MediaDataFrame)>) override {
    return Error("unused");
  }
  Roe<void> Subscribe(uint32_t stream_id, uint16_t channel_id) override {
    subscriptions.emplace_back(stream_id, channel_id);
    return {};
  }
  /** Capture thread (broadcaster) — guarded. */
  Roe<void> SendFrame(const MediaDataFrame& frame) override {
    if (!attached.load()) {
      return Error("not attached");
    }
    std::lock_guard lock(frames_mu);
    sent_frames.push_back(frame);
    return {};
  }
  std::vector<MediaDataFrame> SentFrames() {
    std::lock_guard lock(frames_mu);
    return sent_frames;
  }
  void Detach() override {
    ++detaches;
    attached = false;
    sink = nullptr;
  }
  bool IsAttached() const override { return attached; }
  bool IsLocalHopAttached() const override { return false; }

  void Lose(MediaRelayClientLoss loss = MediaRelayClientLoss::TransportLost) {
    attached = loss == MediaRelayClientLoss::Replaced;  // replaced: someone else holds it now
    for (auto& [token, observer] : std::map<uint64_t, std::function<void(MediaRelayClientLoss)>>(observers)) {
      (void)token;
      observer(loss);
    }
  }

  double rate = 0.0;
  std::atomic<bool> attached{false};
  bool hold_attach = false;
  std::function<void()> held;
  std::string attached_hop;
  std::string last_session;
  MediaRelayQuoteRequest last_quote;
  std::vector<std::string> attach_hops;
  std::unordered_map<std::string, bool> failing_hops;
  std::function<void(MediaDataFrame)> sink;
  std::vector<std::pair<uint32_t, uint16_t>> subscriptions;
  std::map<uint64_t, std::function<void(MediaRelayClientLoss)>> observers;
  uint64_t next_token = 1;
  int reader_starts = 0;
  int detaches = 0;
  std::mutex frames_mu;
  std::vector<MediaDataFrame> sent_frames;
};

inline std::vector<uint8_t> OpusFrame() {
  int err = 0;
  OpusEncoder* enc = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
  std::vector<int16_t> pcm(960, 0);
  std::vector<unsigned char> out(4000);
  const int n = opus_encode(enc, pcm.data(), 960, out.data(), static_cast<int>(out.size()));
  opus_encoder_destroy(enc);
  return {out.begin(), out.begin() + std::max(n, 0)};
}

} // namespace pbr::test
