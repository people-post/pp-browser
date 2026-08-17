#include "base/p2p/CallMediaDirectService.h"
#include "base/p2p/CircuitRelayService.h"
#include "base/p2p/Libp2pHost.h"
#include "base/p2p/Libp2pWorker.h"
#include "base/p2p/PeerSessionManager.h"
#include "base/p2p/StreamFrameIo.h"

#include "common/Logger.h"

#include <libp2p/connection/stream.hpp>
#include <libp2p/connection/stream_and_protocol.hpp>
#include <libp2p/host/host.hpp>
#include <libp2p/peer/protocol.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

void PrintUsage(const char* argv0) {
  std::cerr
      << "Usage:\n"
      << "  " << argv0 << " --role answerer --listen <multiaddr> [--advertise-host <ip>]\n"
      << "                 [--call-id ID] [--ready-file PATH] [--no-auto-detach] [--with-chat]\n"
      << "  " << argv0 << " --role offerer --peer <multiaddr-with-p2p> [--call-id ID] [--cycles K]\n"
      << "                 [--via-hop <multiaddr-with-p2p>] [--hold-ms N] [--timeout-ms N]\n"
      << "                 [--expect ok|busy] [--with-chat]\n"
      << "\n"
      << "Thin-client B-CALL-DIRECT / B-CALL-HOP / B-CONFLICT / B-MSG+CALL\n"
      << "(docs/ops/TEST_STRATEGY.md).\n"
      << "  --expect busy   Connect failure is success (second inbound while MediaReady).\n"
      << "  --hold-ms N     Stay MediaReady after audio before detach (conflict holder).\n"
      << "  --with-chat     Ping /pp-browser/chat/1.0.0 during and after the call.\n";
}

std::optional<std::string> PeerIdFromMultiaddr(const std::string& ma) {
  const auto pos = ma.rfind("/p2p/");
  if (pos == std::string::npos) {
    return std::nullopt;
  }
  auto id = ma.substr(pos + 5);
  while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == '/')) {
    id.pop_back();
  }
  if (id.empty()) {
    return std::nullopt;
  }
  return id;
}

std::string RewriteListenHost(std::string multiaddr, const std::string& host) {
  const std::string from0 = "/ip4/0.0.0.0/";
  const std::string from1 = "/ip4/127.0.0.1/";
  const std::string to = "/ip4/" + host + "/";
  auto pos = multiaddr.find(from0);
  if (pos != std::string::npos) {
    multiaddr.replace(pos, from0.size(), to);
    return multiaddr;
  }
  pos = multiaddr.find(from1);
  if (pos != std::string::npos && host != "127.0.0.1") {
    multiaddr.replace(pos, from1.size(), to);
  }
  return multiaddr;
}

bool HasP2pSuffix(const std::string& ma) {
  return ma.find("/p2p/") != std::string::npos;
}

std::string RewriteWildcardListenHost(std::string multiaddr) {
  const std::string from = "/ip4/0.0.0.0/";
  const std::string to = "/ip4/127.0.0.1/";
  const auto pos = multiaddr.find(from);
  if (pos != std::string::npos) {
    multiaddr.replace(pos, from.size(), to);
  }
  return multiaddr;
}

using libp2p::peer::ProtocolName;

constexpr const char* kProbeChatProtocol = "/pp-browser/chat/1.0.0";

void ArmProbeChat(pbr::Libp2pHost& host, std::atomic<int>& received) {
  host.GetHost().setProtocolHandler(
      {ProtocolName{kProbeChatProtocol}}, [&](libp2p::StreamAndProtocol stream_in) {
        auto stream = std::move(stream_in.stream);
        pbr::PostLibp2pWorker(host, pbr::WorkerLane::Normal, [&received, stream = std::move(stream)]() {
          auto frame = pbr::BlockingReadLengthPrefixedFrame(stream);
          if (!frame) {
            return;
          }
          received.fetch_add(1, std::memory_order_acq_rel);
          (void)pbr::BlockingWriteLengthPrefixedFrame(stream, {'a', 'c', 'k'});
        });
      });
}

bool SendProbeChat(pbr::Libp2pHost& host, pbr::PeerSessionManager& sessions, const std::string& peer_key) {
  auto done = std::make_shared<std::promise<bool>>();
  auto future = done->get_future();
  sessions.OpenStream(peer_key, {ProtocolName{kProbeChatProtocol}},
                      [&host, done](libp2p::StreamAndProtocolOrError stream_res) {
                        if (!stream_res) {
                          done->set_value(false);
                          return;
                        }
                        auto stream = stream_res.value().stream;
                        pbr::PostLibp2pWorker(host, pbr::WorkerLane::Normal, [stream, done]() {
                          const std::vector<uint8_t> ping = {'p', 'i', 'n', 'g'};
                          auto wrote = pbr::BlockingWriteLengthPrefixedFrame(stream, ping);
                          if (!wrote) {
                            done->set_value(false);
                            return;
                          }
                          auto ack = pbr::BlockingReadLengthPrefixedFrame(stream);
                          done->set_value(ack && *ack == std::vector<uint8_t>({'a', 'c', 'k'}));
                        });
                      });
  if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    return false;
  }
  return future.get();
}

int RunAnswerer(const std::string& listen_ma, const std::string& call_id, const std::string& ready_file,
                int hold_seconds, const std::string& advertise_host, bool no_auto_detach, bool with_chat) {
  pbr::Libp2pHostConfig cfg;
  cfg.listen_multiaddr = listen_ma;
  pbr::Libp2pHost host;
  if (auto started = host.Start(cfg); !started) {
    std::cerr << "error: answerer host start: " << started.error().message << "\n";
    return 1;
  }
  auto peer_id = host.LocalPeerIdBase58();
  if (!peer_id) {
    std::cerr << "error: answerer peer id unavailable\n";
    return 1;
  }
  std::string advertise = listen_ma;
  if (!advertise_host.empty()) {
    advertise = RewriteListenHost(std::move(advertise), advertise_host);
  } else {
    advertise = RewriteWildcardListenHost(std::move(advertise));
  }
  if (!HasP2pSuffix(advertise)) {
    advertise += "/p2p/" + *peer_id;
  }

  pbr::PeerSessionConfig sessions_cfg;
  sessions_cfg.dial_timeout = std::chrono::milliseconds(5000);
  sessions_cfg.dial_failure_backoff = std::chrono::milliseconds(100);
  auto sessions = std::make_unique<pbr::PeerSessionManager>(host, sessions_cfg);
  auto media = std::make_unique<pbr::CallMediaDirectService>(host, *sessions);
  media->Start();

  std::atomic<int> chat_received{0};
  if (with_chat) {
    ArmProbeChat(host, chat_received);
  }

  const pbr::ByteVector media_key(32, 0x42);
  std::mutex mu;
  std::condition_variable cv;
  int cycles_done = 0;
  std::atomic<bool> detach_after_audio{false};

  media->SetInboundHandler([&](pbr::CallMediaDirectConnectParams& params, pbr::CallMediaDirectCallbacks& cbs) {
    params.media_key = media_key;
    params.call_id = call_id;
    params.media_epoch = 1;
    params.offerer = false;
    cbs.on_connected = [&] {
      std::lock_guard lock(mu);
      cv.notify_all();
    };
    cbs.on_audio = [&](const std::vector<uint8_t>&) {
      std::lock_guard lock(mu);
      ++cycles_done;
      detach_after_audio.store(true, std::memory_order_release);
      cv.notify_all();
    };
  });

  if (!ready_file.empty()) {
    FILE* f = std::fopen(ready_file.c_str(), "w");
    if (!f) {
      std::cerr << "error: cannot write ready-file " << ready_file << "\n";
      return 1;
    }
    std::fprintf(f, "%s\n", advertise.c_str());
    std::fclose(f);
  }
  std::cout << "pp-call-probe answerer ready peer=" << advertise << "\n";
  std::cout.flush();

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(hold_seconds);
  while (std::chrono::steady_clock::now() < deadline) {
    if (!no_auto_detach && detach_after_audio.exchange(false, std::memory_order_acq_rel)) {
      // Circuit-bridged close does not idle the answerer; Detach so the next cycle can inbound.
      media->Detach();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }

  media->Stop();
  media.reset();
  sessions.reset();
  host.Stop();
  std::cout << "pp-call-probe answerer exit audio_frames=" << cycles_done
            << " chat_frames=" << chat_received.load() << "\n";
  return 0;
}

int RunOfferer(const std::string& peer_ma, const std::string& call_id, int cycles,
               const std::string& hop_ma, int hold_ms, int timeout_ms, bool expect_busy,
               bool with_chat) {
  if (cycles < 1 || cycles > 100) {
    std::cerr << "error: --cycles must be 1..100\n";
    return 2;
  }
  if (!HasP2pSuffix(peer_ma)) {
    std::cerr << "error: --peer must include /p2p/<PeerId>\n";
    return 2;
  }
  const bool via_hop = !hop_ma.empty();
  if (via_hop && !HasP2pSuffix(hop_ma)) {
    std::cerr << "error: --via-hop must include /p2p/<PeerId>\n";
    return 2;
  }
  auto peer_id = PeerIdFromMultiaddr(peer_ma);
  if (!peer_id) {
    std::cerr << "error: cannot parse peer id from --peer\n";
    return 2;
  }

  pbr::Libp2pHostConfig cfg;
  cfg.listen_multiaddr = "/ip4/127.0.0.1/tcp/0";
  pbr::Libp2pHost host;
  if (auto started = host.Start(cfg); !started) {
    std::cerr << "error: offerer host start: " << started.error().message << "\n";
    return 1;
  }

  pbr::PeerSessionConfig sessions_cfg;
  sessions_cfg.dial_timeout = std::chrono::milliseconds(5000);
  sessions_cfg.dial_failure_backoff = std::chrono::milliseconds(100);
  auto sessions = std::make_unique<pbr::PeerSessionManager>(host, sessions_cfg);
  auto circuit = std::make_unique<pbr::CircuitRelayService>(host, *sessions);
  if (via_hop) {
    circuit->Start();
  }
  auto media = std::make_unique<pbr::CallMediaDirectService>(host, *sessions);
  media->Start();

  const std::string peer_key = via_hop ? *peer_id : std::string{"peer"};
  if (via_hop) {
    if (auto reg = sessions->RegisterEndpoint("hop", RewriteWildcardListenHost(hop_ma)); !reg) {
      std::cerr << "error: register hop: " << reg.error().message << "\n";
      return 1;
    }
    std::cout << "pp-call-probe offerer via-hop=" << hop_ma << " peer=" << peer_ma << "\n";
  } else if (auto reg = sessions->RegisterEndpoint("peer", RewriteWildcardListenHost(peer_ma)); !reg) {
    std::cerr << "error: register peer: " << reg.error().message << "\n";
    return 1;
  }

  const pbr::ByteVector media_key(32, 0x42);
  for (int cycle = 0; cycle < cycles; ++cycle) {
    if (via_hop) {
      pbr::CircuitBridgeTarget target;
      target.target_peer_id = *peer_id;
      target.target_multiaddr = peer_ma;
      target.target_protocol = pbr::kCallMediaDirectProtocolId;
      auto bridged = circuit->RequestBridge("hop", target, 10000);
      if (!bridged || !bridged->ok || !bridged->stream) {
        std::cerr << "error: circuit bridge cycle " << cycle << ": "
                  << (bridged ? bridged->error : bridged.error().message) << "\n";
        return 1;
      }
      if (auto hop = sessions->InstallCircuitHop(*peer_id, "hop", pbr::kCallMediaDirectProtocolId,
                                                 bridged->stream);
          !hop) {
        std::cerr << "error: install circuit hop cycle " << cycle << ": " << hop.error().message
                  << "\n";
        return 1;
      }
      if (!sessions->IsCircuitBacked(*peer_id, pbr::kCallMediaDirectProtocolId)) {
        std::cerr << "error: expected circuit-backed call-media after bridge cycle " << cycle << "\n";
        return 1;
      }
    }

    std::mutex mu;
    std::condition_variable cv;
    bool connected = false;

    pbr::CallMediaDirectConnectParams params;
    params.peer_key = peer_key;
    params.call_id = call_id;
    params.media_key = media_key;
    params.media_epoch = 1;
    params.offerer = true;

    pbr::CallMediaDirectCallbacks cbs;
    cbs.on_connected = [&] {
      std::lock_guard lock(mu);
      connected = true;
      cv.notify_one();
    };

    if (auto ok = media->Connect(params, cbs, timeout_ms); !ok) {
      if (expect_busy) {
        std::cout << "ok  busy cycle " << cycle << " connect rejected: " << ok.error().message << "\n";
        continue;
      }
      std::cerr << "error: connect cycle " << cycle << ": " << ok.error().message << "\n";
      return 1;
    }
    {
      std::unique_lock lock(mu);
      if (!cv.wait_for(lock, std::chrono::milliseconds(timeout_ms + 1000), [&] { return connected; })) {
        if (expect_busy) {
          std::cout << "ok  busy cycle " << cycle << " connect timeout\n";
          media->Detach();
          continue;
        }
        std::cerr << "error: connect timeout cycle " << cycle << "\n";
        return 1;
      }
    }
    if (expect_busy) {
      std::cerr << "error: expected busy (second inbound rejected) but connect succeeded\n";
      media->Detach();
      return 1;
    }

    const std::vector<uint8_t> opus = {static_cast<uint8_t>(0x10 + cycle)};
    if (!media->SendAudio(opus, 1, 0)) {
      std::cerr << "error: send audio cycle " << cycle << "\n";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (with_chat) {
      if (!SendProbeChat(host, *sessions, peer_key)) {
        std::cerr << "error: chat during call cycle " << cycle << "\n";
        return 1;
      }
      std::cout << "ok  chat during call cycle " << cycle << "\n";
      if (!media->SendAudio({static_cast<uint8_t>(0x20 + cycle)}, 2, 0)) {
        std::cerr << "error: audio after chat cycle " << cycle << "\n";
        return 1;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    if (hold_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(hold_ms));
    }
    media->Detach();
    if (via_hop) {
      sessions->ClearCircuitHop(*peer_id, pbr::kCallMediaDirectProtocolId);
      std::this_thread::sleep_for(std::chrono::milliseconds(800));
    }
    if (with_chat) {
      if (!SendProbeChat(host, *sessions, peer_key)) {
        std::cerr << "error: chat after leave cycle " << cycle << "\n";
        return 1;
      }
      std::cout << "ok  chat after leave cycle " << cycle << "\n";
    }
    std::cout << "ok  cycle " << cycle << " connect+audio+detach"
              << (via_hop ? " via-hop" : "") << "\n";
  }

  media->Stop();
  media.reset();
  circuit.reset();
  sessions.reset();
  host.Stop();
  std::cout << "pp-call-probe offerer PASSED cycles=" << cycles
            << (via_hop ? " via-hop" : "") << "\n";
  return 0;
}

} // namespace

int main(int argc, char** argv) {
  std::string role;
  std::string listen_ma = "/ip4/127.0.0.1/tcp/47100";
  std::string peer_ma;
  std::string hop_ma;
  std::string advertise_host;
  std::string call_id = "pp-call-direct";
  std::string ready_file;
  int cycles = 1;
  int hold_seconds = 30;
  int hold_ms = 0;
  int timeout_ms = 8000;
  bool expect_busy = false;
  bool with_chat = false;
  bool no_auto_detach = false;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      PrintUsage(argv[0]);
      return 0;
    }
    if (std::strcmp(argv[i], "--role") == 0 && i + 1 < argc) {
      role = argv[++i];
    } else if (std::strcmp(argv[i], "--listen") == 0 && i + 1 < argc) {
      listen_ma = argv[++i];
    } else if (std::strcmp(argv[i], "--peer") == 0 && i + 1 < argc) {
      peer_ma = argv[++i];
    } else if (std::strcmp(argv[i], "--via-hop") == 0 && i + 1 < argc) {
      hop_ma = argv[++i];
    } else if (std::strcmp(argv[i], "--advertise-host") == 0 && i + 1 < argc) {
      advertise_host = argv[++i];
    } else if (std::strcmp(argv[i], "--call-id") == 0 && i + 1 < argc) {
      call_id = argv[++i];
    } else if (std::strcmp(argv[i], "--ready-file") == 0 && i + 1 < argc) {
      ready_file = argv[++i];
    } else if (std::strcmp(argv[i], "--cycles") == 0 && i + 1 < argc) {
      cycles = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--hold-seconds") == 0 && i + 1 < argc) {
      hold_seconds = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc) {
      hold_ms = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--timeout-ms") == 0 && i + 1 < argc) {
      timeout_ms = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--expect") == 0 && i + 1 < argc) {
      ++i;
      if (std::strcmp(argv[i], "busy") == 0) {
        expect_busy = true;
      } else if (std::strcmp(argv[i], "ok") == 0) {
        expect_busy = false;
      } else {
        std::cerr << "error: --expect ok|busy\n";
        return 2;
      }
    } else if (std::strcmp(argv[i], "--with-chat") == 0) {
      with_chat = true;
    } else if (std::strcmp(argv[i], "--no-auto-detach") == 0) {
      no_auto_detach = true;
    } else {
      std::cerr << "Unknown argument: " << argv[i] << "\n";
      PrintUsage(argv[0]);
      return 2;
    }
  }

  auto root = pbr::logging::getRootLogger();
  root.setLevel(pbr::logging::Level::INFO);

  if (const char* env = std::getenv("PP_CALL_PROBE_VIA_HOP")) {
    if (hop_ma.empty() && env[0] != '\0') {
      hop_ma = env;
    }
  }
  if (const char* env = std::getenv("PP_NODE_PROBE_ADVERTISE_HOST")) {
    if (advertise_host.empty() && env[0] != '\0') {
      advertise_host = env;
    }
  }

  if (role == "answerer") {
    return RunAnswerer(listen_ma, call_id, ready_file, hold_seconds, advertise_host, no_auto_detach,
                       with_chat);
  }
  if (role == "offerer") {
    if (peer_ma.empty()) {
      std::cerr << "error: --peer required for offerer\n";
      return 2;
    }
    return RunOfferer(peer_ma, call_id, cycles, hop_ma, hold_ms, timeout_ms, expect_busy, with_chat);
  }
  std::cerr << "error: --role answerer|offerer required\n";
  PrintUsage(argv[0]);
  return 2;
}
