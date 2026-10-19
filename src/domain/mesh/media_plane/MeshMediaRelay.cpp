#include "domain/mesh/media_plane/MeshMediaRelay.h"

#include "domain/mesh/l4/media_relay/client/AmpMediaRelayClient.h"
#include "domain/mesh/shared/MeshThreads.h"

#include <utility>
#include "common/PbrCompat.h"

namespace pbr {
namespace {

/** Its owner: the Connectivity owner, next to `MeshConnectivity` (thread-ownership T001). */

} // namespace

MeshMediaRelay::MeshMediaRelay(MeshConnectivity& connectivity) : connectivity_(connectivity) {
  redirectLogger("MeshMediaRelay");
}

MeshMediaRelay::~MeshMediaRelay() {
  Clear();
}

void MeshMediaRelay::Wire() {
  MeshThreads::RunOnOwnerAndWait([&]() {
    connectivity_.InvalidateObjects();  // rewire replaces the client earlier ports point at
    MeshHost* m = connectivity_.Mesh();
    if (!m || !AmpRelayAvailable()) {
      media_relay_client_.reset();
      log().warning << "media-relay transport unavailable (Amp required)";
      return;
    }
    MeshIoContext io;
    if (auto chat = m->ChatDeps()) {
      // Exclusive Amp Drive: io_pump is empty; MeshPump (or a harness Tick loop) progresses Amp.
      io = chat->io;
    }
    media_relay_client_ = std::make_unique<AmpMediaRelayClient>(*m->AmpMediaRelayClientCoord(), io.io_pump,
                                                                m->Amp()->LocalPeerId(), io.post_io, io.post_after);
    log().info << "media-relay transport=amp";
  });
}

void MeshMediaRelay::BindTestRelay(IMediaRelayClient* relay) {
  MeshThreads::RunOnOwnerAndWait([&]() { test_relay_ = relay; });
}

void MeshMediaRelay::ResetRelayClient() {
  MeshThreads::RunOnOwnerAndWait([&]() {
    connectivity_.InvalidateObjects();
    media_relay_client_.reset();
  });
}

void MeshMediaRelay::Clear() {
  MeshThreads::RunOnOwnerAndWait([&]() {
    connectivity_.InvalidateObjects();
    media_relay_client_.reset();
    test_relay_ = nullptr;
  });
}

bool MeshMediaRelay::AmpRelayAvailable() const {
  MeshHost* m = connectivity_.Mesh();
  return m && m->Amp() && m->AmpMediaRelayClientCoord() && m->AmpMediaRelayClientCoord()->IsStarted();
}

MediaRelayAttachPorts MeshMediaRelay::RelayAttachPorts() const {
  MediaRelayAttachPorts ports;
  ports.relay = RelayClient();
  ports.dial = connectivity_.Dial();
  ports.service_reach = connectivity_.CircuitReach();
  ports.objects_alive = connectivity_.ObjectsToken();
  ports.objects_snap = connectivity_.ObjectsSnapshot();
  return ports;
}

} // namespace pbr
