# libp2p hard fork

**Tier:** architecture

pp-browser vendors [cpp-libp2p](https://github.com/libp2p/cpp-libp2p) under `src/lib/libp2p/` as a **hard fork** (committed source, no git submodule).

**A017 (2026-08-31):** Host / TCP / Yamux / Noise / QUIC / Identify / Kademlia / Gossip were **deleted**. The fork retains **PeerId + key wire** only (`p2p_peer_id`, `p2p_wire`). Product mesh is Amp (`lib/amp`).

## Layout

| Path | Role |
|------|------|
| `src/lib/libp2p/` | Shrunk cpp-libp2p (`include/` + `src/` PeerId/crypto/multi/wire/log) |
| `src/base/p2p/` | Amp mesh glue (`MeshHost`, PeerIdUtil, L4 coordinators) — **not** Libp2pHost |

Dependency rule:

```
base/p2p → lib/libp2p/include (PeerId + keys_wire)
lib/libp2p/src → lib/libp2p/include
```

## Provenance

See [`src/lib/libp2p/UPSTREAM.json`](../../src/lib/libp2p/UPSTREAM.json) for the upstream commit SHA.

Imported from upstream commit `28e4abcea0bf3fb1b04e51febfea38305f101fe7` (2026-06-13), then A017-shrunk.

## Product profile

[`src/lib/pp_lib_libp2p.cmake`](../../src/lib/pp_lib_libp2p.cmake): `PACKAGE_MANAGER=vendored`, clang-tidy/format off, no Host tree. No `PP_BROWSER_LIBP2P_*` knobs.

## Dependency management

Upstream used Hunter; pp-browser **removed Hunter** and vendors dependencies under `third_party/`:

- Import script: [`scripts/libp2p_vendor_import.sh`](../../scripts/libp2p_vendor_import.sh)
- CMake wiring: [`cmake/libp2p_dependencies.cmake`](../cmake/libp2p_dependencies.cmake)
- Versions recorded in [`third_party/UPSTREAM.json`](../../third_party/UPSTREAM.json) under `libp2p_dependencies`

**PeerId-only link set:** BoringSSL (SHA), qtils, soralog, fmt, yaml-cpp, Outcome. Asio remains for `pp-node` StatusHttpServer (not libp2p Host). **Removed from `third_party/`:** lsquic, c-ares, tsl_hat_trie.

## What we keep vs drop

| Keep | Dropped (A017) |
|------|----------------|
| `p2p_peer_id` (ML-DSA PeerId via `keys_wire`) | Host, ExplicitHost, BasicHost |
| `p2p_wire` (`keys_wire` + codec) | TCP / QUIC transport, Yamux, Noise, multistream |
| multihash / multibase / SHA | Identify, Kademlia, Gossip, ping, echo |
| | Address/key/protocol repositories |

## Integration status

Product mesh is Amp ([adp](../../projects/adp/), [NETWORKING.md](NETWORKING.md)). `PeerIdUtil` derives base58 PeerId from device ML-DSA-65 public keys via `p2p_peer_id` + `keys_wire`.

### Full-PQ identity (libp2p-pq-transport hangover)

| Item | Value |
|------|-------|
| Device identity `KeyType` wire | `MlDsa65 = 4` (provisional) |
| Noise suite | **Retired** with TCP Host (A017) |
| Planning | [projects/libp2p-pq-transport/](../../projects/libp2p-pq-transport/) |

L4 protocols now run on Amp ChannelSession — see [AMP-CHANNEL.md](../contracts/AMP-CHANNEL.md) and [adp CURRENT_STATE](../../projects/adp/CURRENT_STATE.md).
