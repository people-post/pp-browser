#include "feature/calls/CallHopMediaChannel.h"

#include "domain/call_media/CallMediaFrameCrypto.h"
#include "mesh/types/MediaChannel.h"
#include "common/Utilities.h"

#include <utility>
#include "common/PbrCompat.h"
#include "common/MeshAlias.h"

namespace pbr {

CallHopMediaChannel::CallHopMediaChannel(std::string call_id, std::shared_ptr<CallEngine> engine,
                                         const CallHopMediaRelayDeps& relay)
    : call_id_(std::move(call_id)), engine_(std::move(engine)), relay_(relay) {
  redirectLogger("CallHopMediaChannel");
}

void CallHopMediaChannel::MakeLive(std::shared_ptr<SfuKeyRing> keys) {
  std::lock_guard lock(mu_);
  live_keys_ = std::move(keys);
}

bool CallHopMediaChannel::HasLiveKeys() const {
  std::lock_guard lock(mu_);
  return live_keys_ != nullptr;
}

void CallHopMediaChannel::InstallKey(const uint32_t epoch, const ByteVector& key) {
  std::shared_ptr<SfuKeyRing> live;
  {
    std::lock_guard lock(mu_);
    live = live_keys_;
  }
  if (live) {
    live->Install(epoch, key, util::NowUnixMs());
  }
}

std::function<void(mesh::MediaDataFrame)> CallHopMediaChannel::MakeFrameSink(std::shared_ptr<SfuKeyRing> keys,
                                                                             std::shared_ptr<std::atomic<bool>> ready) {
  return [self = shared_from_this(), ready = std::move(ready), keys = std::move(keys)](mesh::MediaDataFrame frame) {
    if (!ready->load(std::memory_order_acquire)) {
      return;
    }
    MediaEngine::MediaPacket pkt;
    pkt.stream_id = frame.stream_id;
    pkt.channel_id = frame.channel_id;
    pkt.seq = frame.seq;
    pkt.mark = frame.mark;
    const std::vector<SfuKeyRing::Key> open_with = keys->ForOpen(util::NowUnixMs());
    if (open_with.empty()) {
      pkt.payload = std::move(frame.payload);
      (void)self->engine_->Deliver(pkt);
      return;
    }
    // The newest epoch first; the previous one while the others switch to it.
    Roe<CallMediaDecodedFrame> plain = Error("no key opened the frame");
    for (const SfuKeyRing::Key& key : open_with) {
      plain = DecryptCallMediaSfuFrame(key.key, self->call_id_, key.epoch, frame.stream_id,
                                       static_cast<uint8_t>(frame.channel_id), frame.payload);
      if (plain) {
        break;
      }
    }
    if (!plain) {
      static std::atomic<int> decrypt_fail_log{0};
      const int n = decrypt_fail_log.fetch_add(1, std::memory_order_relaxed);
      if (n < 8 || (n % 100) == 0) {
        self->log().warning << "SFU decrypt failed stream=" << frame.stream_id << " ch=" << frame.channel_id
                            << " bytes=" << frame.payload.size() << " n=" << n << " err=" << plain.error().message
                            << " call_id=" << self->call_id_;
      }
      return;
    }
    pkt.payload = std::move(plain->payload);
    (void)self->engine_->Deliver(pkt);
  };
}

MediaEngine::PacketSendFn CallHopMediaChannel::MakeSendFn(std::shared_ptr<SfuKeyRing> keys) {
  return
      [self = shared_from_this(), pub = LocalStreamId(), keys = std::move(keys)](const MediaEngine::MediaPacket& pkt) {
        if (!self->relay_.relay) {
          return;
        }
        mesh::MediaDataFrame frame;
        frame.stream_id = pub;
        frame.channel_id = pkt.channel_id;
        frame.channel_type = mesh::MediaChannelTypeFor(pkt.channel_id);
        frame.seq = pkt.seq;
        frame.mark = pkt.mark;
        const SfuKeyRing::Key key = keys->Current(); // the newest epoch (V003)
        if (!key.key.empty()) {
          auto sealed = EncryptCallMediaSfuFrame(key.key, self->call_id_, key.epoch, pub, pkt.seq, pkt.mark,
                                                 static_cast<uint8_t>(pkt.channel_id), pkt.payload);
          if (!sealed) {
            self->engine_->NoteOutboundDrop();
            return;
          }
          frame.payload = std::move(*sealed);
        } else {
          frame.payload = pkt.payload;
        }
        if (!self->relay_.relay->SendFrame(frame)) {
          self->engine_->NoteOutboundDrop();
        }
      };
}

void CallHopMediaChannel::AddRemoteStream(const uint32_t id) {
  std::lock_guard lock(mu_);
  remote_stream_ids_.insert(id);
}

std::vector<uint32_t> CallHopMediaChannel::RemoteStreams() const {
  std::lock_guard lock(mu_);
  return {remote_stream_ids_.begin(), remote_stream_ids_.end()};
}

size_t CallHopMediaChannel::RemoteStreamCount() const {
  std::lock_guard lock(mu_);
  return remote_stream_ids_.size();
}

bool CallHopMediaChannel::FirstVideoRefresh(const uint32_t stream_id) {
  std::lock_guard lock(mu_);
  return video_refresh_sent_.insert(stream_id).second;
}

void CallHopMediaChannel::ResetStreams() {
  local_stream_id_.store(0);
  std::lock_guard lock(mu_);
  remote_stream_ids_.clear();
}

} // namespace pbr
