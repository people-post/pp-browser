#include "base/media/CallMediaEngine.h"

#include "common/Utilities.h"

#include <SDL3/SDL.h>
#include <opus.h>
#include <rtc/rtc.hpp>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <deque>
#include <thread>
#include <vector>

namespace pbr {
namespace {

constexpr int kSampleRate = 48000;
constexpr int kChannels = 1;
constexpr int kFrameMs = 20;
constexpr int kFrameSamples = kSampleRate * kFrameMs / 1000; // 960
constexpr int kOpusPayloadType = 111;
// Each peer must advertise a distinct send SSRC (SDP: local description lists only SSRCs we send).
constexpr rtc::SSRC kOffererAudioSsrc = 1;
constexpr rtc::SSRC kAnswererAudioSsrc = 2;

std::string StateToString(rtc::PeerConnection::State state) {
  switch (state) {
  case rtc::PeerConnection::State::New:
    return "new";
  case rtc::PeerConnection::State::Connecting:
    return "connecting";
  case rtc::PeerConnection::State::Connected:
    return "connected";
  case rtc::PeerConnection::State::Disconnected:
    return "disconnected";
  case rtc::PeerConnection::State::Failed:
    return "failed";
  case rtc::PeerConnection::State::Closed:
    return "closed";
  }
  return "unknown";
}

} // namespace

struct CallMediaEngine::Impl {
  std::mutex mutex;
  std::string call_id;
  Role role = Role::Offerer;
  std::atomic<bool> active{false};
  std::atomic<bool> connected{false};
  std::string connection_state = "idle";

  LocalDescriptionFn on_local_description;
  IceCandidateFn on_ice_candidate;
  StateChangedFn on_state_changed;

  std::shared_ptr<rtc::PeerConnection> pc;
  std::shared_ptr<rtc::Track> track;
  std::shared_ptr<rtc::RtpPacketizationConfig> rtp_config;
  rtc::SSRC local_ssrc = kOffererAudioSsrc;
  bool capture_available = false;

  OpusEncoder* encoder = nullptr;
  OpusDecoder* decoder = nullptr;
  SDL_AudioStream* capture_stream = nullptr;
  SDL_AudioStream* playback_stream = nullptr;
  SDL_AudioDeviceID capture_device = 0;
  SDL_AudioDeviceID playback_device = 0;

  std::thread capture_thread;
  std::atomic<bool> capture_running{false};
  std::atomic<float> local_input_level{0.f};
  std::atomic<float> remote_output_level{0.f};
  std::atomic<int64_t> remote_level_ms{0};

  std::mutex playback_mutex;
  std::deque<std::vector<int16_t>> playback_queue;

  static float FramePeakLevel(const int16_t* pcm, int samples) {
    int peak = 0;
    for (int i = 0; i < samples; ++i) {
      peak = std::max(peak, std::abs(static_cast<int>(pcm[i])));
    }
    return std::clamp(static_cast<float>(peak) / 32768.f, 0.f, 1.f);
  }

  static void SmoothLevel(std::atomic<float>& level, float instant) {
    const float cur = level.load(std::memory_order_relaxed);
    // Fast attack, slower decay so speaking lights up quickly and fades cleanly.
    const float next = instant >= cur ? instant : (cur * 0.82f + instant * 0.18f);
    level.store(std::clamp(next, 0.f, 1.f), std::memory_order_relaxed);
  }

  void SetState(const std::string& state) {
    connection_state = state;
    connected = (state == "connected");
    if (on_state_changed) {
      on_state_changed(state);
    }
  }

  void TearDownAudioLocked() {
    capture_running = false;
    if (capture_thread.joinable()) {
      capture_thread.join();
    }
    if (capture_stream) {
      SDL_DestroyAudioStream(capture_stream);
      capture_stream = nullptr;
    }
    if (playback_stream) {
      SDL_DestroyAudioStream(playback_stream);
      playback_stream = nullptr;
    }
    if (capture_device) {
      SDL_CloseAudioDevice(capture_device);
      capture_device = 0;
    }
    if (playback_device) {
      SDL_CloseAudioDevice(playback_device);
      playback_device = 0;
    }
    if (encoder) {
      opus_encoder_destroy(encoder);
      encoder = nullptr;
    }
    if (decoder) {
      opus_decoder_destroy(decoder);
      decoder = nullptr;
    }
    {
      std::lock_guard lock(playback_mutex);
      playback_queue.clear();
    }
    local_input_level.store(0.f, std::memory_order_relaxed);
    remote_output_level.store(0.f, std::memory_order_relaxed);
    remote_level_ms.store(0, std::memory_order_relaxed);
    capture_available = false;
  }

  void TearDownPcLocked() {
    if (track) {
      track->onMessage(nullptr);
      track.reset();
    }
    rtp_config.reset();
    if (pc) {
      pc->close();
      pc.reset();
    }
  }

  Roe<void> EnsureAudioSubsystem() {
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
      if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        return Error(std::string("SDL_InitSubSystem(AUDIO) failed: ") + SDL_GetError());
      }
    }
    return {};
  }

  Roe<void> OpenAudioDevices() {
    if (auto ok = EnsureAudioSubsystem(); !ok) {
      return ok.error();
    }

    int err = 0;
    encoder = opus_encoder_create(kSampleRate, kChannels, OPUS_APPLICATION_VOIP, &err);
    if (!encoder || err != OPUS_OK) {
      return Error("opus_encoder_create failed");
    }
    opus_encoder_ctl(encoder, OPUS_SET_BITRATE(24000));
    decoder = opus_decoder_create(kSampleRate, kChannels, &err);
    if (!decoder || err != OPUS_OK) {
      return Error("opus_decoder_create failed");
    }

    SDL_AudioSpec want{};
    want.freq = kSampleRate;
    want.format = SDL_AUDIO_S16;
    want.channels = static_cast<Uint8>(kChannels);

    // Mic is optional: headless / no-capture hosts can still receive and play audio.
    capture_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_RECORDING, &want, nullptr, nullptr);
    capture_available = false;
    if (capture_stream) {
      capture_device = SDL_GetAudioStreamDevice(capture_stream);
      if (SDL_ResumeAudioDevice(capture_device)) {
        capture_available = true;
      } else {
        SDL_DestroyAudioStream(capture_stream);
        capture_stream = nullptr;
        capture_device = 0;
      }
    }

    playback_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, nullptr, nullptr);
    if (!playback_stream) {
      return Error(std::string("SDL playback open failed: ") + SDL_GetError());
    }
    playback_device = SDL_GetAudioStreamDevice(playback_stream);
    if (!SDL_ResumeAudioDevice(playback_device)) {
      return Error(std::string("SDL playback resume failed: ") + SDL_GetError());
    }
    return {};
  }

  void BindAudioTrack(const std::shared_ptr<rtc::Track>& audio_track, rtc::SSRC ssrc) {
    local_ssrc = ssrc;
    rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(ssrc, "audio", kOpusPayloadType,
                                                               rtc::OpusRtpPacketizer::DefaultClockRate);
    auto packetizer = std::make_shared<rtc::OpusRtpPacketizer>(rtp_config);
    auto depacketizer = std::make_shared<rtc::OpusRtpDepacketizer>();
    auto rtcp_recv = std::make_shared<rtc::RtcpReceivingSession>();
    auto sr_reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);
    // Packetizer (out) → depacketizer (in) → RTCP RR/SR for a healthy SendRecv track.
    packetizer->addToChain(depacketizer);
    packetizer->addToChain(rtcp_recv);
    packetizer->addToChain(sr_reporter);
    audio_track->setMediaHandler(packetizer);
    audio_track->onMessage(
        [this](rtc::binary data) {
          if (!data.empty()) {
            OnRemoteOpusFrame(data.data(), data.size());
          }
        },
        nullptr);
    track = audio_track;
  }

  void StartCaptureLoop() {
    capture_running = true;
    capture_thread = std::thread([this]() {
      std::vector<int16_t> pcm(static_cast<size_t>(kFrameSamples));
      std::vector<int16_t> pending;
      pending.reserve(static_cast<size_t>(kFrameSamples) * 2);
      std::vector<unsigned char> opus_buf(4000);
      while (capture_running.load()) {
        const bool can_send = track && track->isOpen() && encoder;
        if (capture_stream) {
          // WASAPI/Pulse often return partial chunks — accumulate to one Opus frame.
          int16_t chunk[kFrameSamples];
          const int got = SDL_GetAudioStreamData(capture_stream, chunk,
                                                 static_cast<int>(sizeof(chunk)));
          if (got > 0) {
            const size_t samples = static_cast<size_t>(got) / sizeof(int16_t);
            pending.insert(pending.end(), chunk, chunk + samples);
          }
          if (pending.size() < static_cast<size_t>(kFrameSamples)) {
            if (got <= 0) {
              SmoothLevel(local_input_level, 0.f);
              const int64_t now = util::NowUnixMs();
              if (now - remote_level_ms.load(std::memory_order_relaxed) > 40) {
                SmoothLevel(remote_output_level, 0.f);
              }
              std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            continue;
          }
          std::copy_n(pending.begin(), kFrameSamples, pcm.begin());
          pending.erase(pending.begin(), pending.begin() + kFrameSamples);
          SmoothLevel(local_input_level, FramePeakLevel(pcm.data(), kFrameSamples));
        } else {
          // No mic: send silence so the peer track stays alive / comfort noise path.
          std::fill(pcm.begin(), pcm.end(), 0);
          SmoothLevel(local_input_level, 0.f);
          if (!can_send) {
            const int64_t now = util::NowUnixMs();
            if (now - remote_level_ms.load(std::memory_order_relaxed) > 40) {
              SmoothLevel(remote_output_level, 0.f);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kFrameMs));
            continue;
          }
        }
        {
          const int64_t now = util::NowUnixMs();
          if (now - remote_level_ms.load(std::memory_order_relaxed) > 40) {
            SmoothLevel(remote_output_level, 0.f);
          }
        }
        if (!can_send) {
          continue;
        }
        const int encoded =
            opus_encode(encoder, pcm.data(), kFrameSamples, opus_buf.data(), static_cast<int>(opus_buf.size()));
        if (encoded <= 0) {
          continue;
        }
        try {
          track->send(reinterpret_cast<const std::byte*>(opus_buf.data()), static_cast<size_t>(encoded));
          if (rtp_config) {
            rtp_config->timestamp += static_cast<uint32_t>(kFrameSamples);
          }
        } catch (...) {
          // Peer may be mid-renegotiation; drop frame.
        }
        if (!capture_stream) {
          std::this_thread::sleep_for(std::chrono::milliseconds(kFrameMs));
        }
      }
    });
  }

  void OnRemoteOpusFrame(const std::byte* data, size_t size) {
    if (!decoder || size == 0) {
      return;
    }
    std::vector<int16_t> pcm(static_cast<size_t>(kFrameSamples));
    const int decoded =
        opus_decode(decoder, reinterpret_cast<const unsigned char*>(data), static_cast<int>(size), pcm.data(),
                    kFrameSamples, 0);
    if (decoded <= 0) {
      return;
    }
    SmoothLevel(remote_output_level, FramePeakLevel(pcm.data(), decoded));
    remote_level_ms.store(util::NowUnixMs(), std::memory_order_relaxed);
    if (!playback_stream) {
      return;
    }
    (void)SDL_PutAudioStreamData(playback_stream, pcm.data(), decoded * static_cast<int>(sizeof(int16_t)));
  }

  Roe<void> SetupPeerConnection(Role start_role) {
    rtc::Configuration config;
    // LAN dogfood: host candidates only (no STUN/TURN until mesh SFU).
    config.enableIceTcp = false;
    // We create offer/answer explicitly (offerer at Start; answerer after remote offer).
    // Leaving auto-negotiation on would answer inside setRemoteDescription, then our
    // second setLocalDescription(Answer) throws "Unexpected local … answer in … stable".
    config.disableAutoNegotiation = true;
    pc = std::make_shared<rtc::PeerConnection>(config);
    role = start_role;
    local_ssrc = (start_role == Role::Offerer) ? kOffererAudioSsrc : kAnswererAudioSsrc;

    pc->onStateChange([this](rtc::PeerConnection::State state) { SetState(StateToString(state)); });

    pc->onLocalCandidate([this](rtc::Candidate candidate) {
      if (!on_ice_candidate) {
        return;
      }
      IceCandidate ice;
      ice.candidate = std::string(candidate);
      ice.mid = candidate.mid();
      if (ice.mid.empty()) {
        ice.mid = "audio";
      }
      on_ice_candidate(ice);
    });

    pc->onLocalDescription([this](rtc::Description description) {
      if (!on_local_description) {
        return;
      }
      LocalDescription local;
      local.type = description.typeString();
      local.sdp = std::string(description);
      on_local_description(local);
    });

    // Answerer: SDP answer must list only SSRCs we send. After remote offer, onTrack fires with
    // the negotiated m-line (remote SSRCs cleared) — re-add our send SSRC before answering.
    // Offerer: same callback can fire on renegotiation; keep handlers on the negotiated track.
    pc->onTrack([this](std::shared_ptr<rtc::Track> remote_track) {
      if (!remote_track) {
        return;
      }
      auto desc = remote_track->description();
      desc.addSSRC(local_ssrc, "audio");
      remote_track->setDescription(desc);
      BindAudioTrack(remote_track, local_ssrc);
    });

    if (start_role == Role::Offerer) {
      rtc::Description::Audio media("audio", rtc::Description::Direction::SendRecv);
      media.addOpusCodec(kOpusPayloadType);
      media.addSSRC(local_ssrc, "audio");
      auto local_track = pc->addTrack(media);
      BindAudioTrack(local_track, local_ssrc);
      pc->setLocalDescription();
    } else {
      // Answerer: also seed a local SendRecv m-line so mid="audio" exists before the offer.
      // processLocalDescription prefers this track and keeps our SSRC in the answer.
      rtc::Description::Audio media("audio", rtc::Description::Direction::SendRecv);
      media.addOpusCodec(kOpusPayloadType);
      media.addSSRC(local_ssrc, "audio");
      auto local_track = pc->addTrack(media);
      BindAudioTrack(local_track, local_ssrc);
    }
    return {};
  }
};

CallMediaEngine::CallMediaEngine() : impl_(std::make_unique<Impl>()) {
  redirectLogger("CallMediaEngine");
}

CallMediaEngine::~CallMediaEngine() {
  Stop();
}

void CallMediaEngine::SetOnLocalDescription(LocalDescriptionFn callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->on_local_description = std::move(callback);
}

void CallMediaEngine::SetOnIceCandidate(IceCandidateFn callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->on_ice_candidate = std::move(callback);
}

void CallMediaEngine::SetOnStateChanged(StateChangedFn callback) {
  std::lock_guard lock(impl_->mutex);
  impl_->on_state_changed = std::move(callback);
}

Roe<void> CallMediaEngine::Start(const std::string& call_id, const Role role) {
  std::lock_guard lock(impl_->mutex);
  if (call_id.empty()) {
    return Error("call_id required");
  }
  if (impl_->active) {
    if (impl_->call_id == call_id) {
      return {};
    }
    return Error("Call media engine already active");
  }
  if (auto audio = impl_->OpenAudioDevices(); !audio) {
    impl_->TearDownAudioLocked();
    return audio.error();
  }
  if (!impl_->capture_available) {
    log().warning << "Call media started without capture device — sending silence; playback still active";
  }
  if (auto pc = impl_->SetupPeerConnection(role); !pc) {
    impl_->TearDownAudioLocked();
    return pc.error();
  }
  impl_->call_id = call_id;
  impl_->active = true;
  impl_->SetState("connecting");
  impl_->StartCaptureLoop();
  return {};
}

Roe<void> CallMediaEngine::SetRemoteDescription(const std::string& type, const std::string& sdp) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->pc) {
    return Error("Media peer connection not started");
  }
  try {
    rtc::Description remote(sdp, type);
    impl_->pc->setRemoteDescription(remote);
    if (impl_->role == Role::Answerer && type == "offer") {
      impl_->pc->setLocalDescription(rtc::Description::Type::Answer);
    }
  } catch (const std::exception& ex) {
    return Error(std::string("setRemoteDescription failed: ") + ex.what());
  }
  return {};
}

Roe<void> CallMediaEngine::AddRemoteIceCandidate(const std::string& candidate, const std::string& mid) {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->pc) {
    return Error("Media peer connection not started");
  }
  try {
    rtc::Candidate ice(candidate, mid.empty() ? "audio" : mid);
    impl_->pc->addRemoteCandidate(ice);
  } catch (const std::exception& ex) {
    return Error(std::string("addRemoteCandidate failed: ") + ex.what());
  }
  return {};
}

void CallMediaEngine::Stop() {
  std::lock_guard lock(impl_->mutex);
  if (!impl_->active && !impl_->pc) {
    return;
  }
  impl_->active = false;
  impl_->TearDownAudioLocked();
  impl_->TearDownPcLocked();
  impl_->call_id.clear();
  impl_->SetState("closed");
}

bool CallMediaEngine::IsActive() const {
  return impl_->active.load();
}

bool CallMediaEngine::IsConnected() const {
  return impl_->connected.load();
}

std::string CallMediaEngine::ActiveCallId() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->call_id;
}

std::string CallMediaEngine::ConnectionState() const {
  std::lock_guard lock(impl_->mutex);
  return impl_->connection_state;
}

float CallMediaEngine::LocalInputLevel() const {
  return impl_->local_input_level.load(std::memory_order_relaxed);
}

float CallMediaEngine::RemoteOutputLevel() const {
  return impl_->remote_output_level.load(std::memory_order_relaxed);
}

} // namespace pbr
