#pragma once

namespace pbr {

/** Per-service async data-plane toggles (temporary — remove after migration). */
struct Libp2pExecutorConfig {
  /** Circuit relay data plane uses host io StreamBridge (default on — legacy worker pumps deadlock). */
  bool async_data_plane_circuit_relay = true;
  bool async_data_plane_media_relay = false;
  bool async_data_plane_dial_back = false;
};

} // namespace pbr
