#include "base/p2p/AdvertisedAddrPublisher.h"

#include "base/p2p/IdentifyIntegrationService.h"
#include "base/p2p/Libp2pHost.h"

namespace pbr {

void PublishAdvertisedListenSet(Libp2pHost& host, IdentifyIntegrationService& identify,
                                const ReachabilitySnapshot& snapshot,
                                const std::string& bound_listen_multiaddr,
                                const std::string& local_peer_id_base58, bool publish_enabled) {
  if (!publish_enabled || !host.IsRunning() || !identify.IsStarted()) {
    return;
  }
  if (local_peer_id_base58.empty()) {
    return;
  }

  const auto ipv6_addrs = EnumerateGlobalIpv6Addresses();
  const std::vector<std::string> advertised =
      BuildAdvertisedListenSet(snapshot.signals, bound_listen_multiaddr, local_peer_id_base58, ipv6_addrs);
  if (advertised.empty()) {
    return;
  }

  host.Post([advertised, &identify]() { (void)identify.PublishSelfAdvertisedAddrs(advertised); });
}

} // namespace pbr
