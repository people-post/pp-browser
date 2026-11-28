# Mesh identity (PeerId)

**Status:** One implementation, in [pp-cpp-crypto](https://github.com/people-post/pp-cpp-crypto)
(`crypto/PeerId.h`, `pp::PeerIdFromMlDsaPublicKey`, v1.0.0) — shared by pp-cpp-amp's links,
pp-browser and pp-ledger. pp-browser's in-tree copy (`foundation/identity`, from the deleted libp2p
fork) is gone.

## Scope

Derives libp2p-shaped base58 PeerIds from ML-DSA-65 device public keys (1952 bytes). Wire format is unchanged:

- Multiaddr segment: `/p2p/<PeerId>`
- JSON call control field: `libp2p_peer_id` (name retained for compat)
- An HTTPS relay's transport key: its PeerId, derived the same way, is the audience every signed relay command names
  (multi-relay MR007). It is **not** the relay's address: that is its relay id ([§ Relay links](#relay-links)), and its
  certificate proves which PeerId is the audience ([SERVICE_ENDPOINTS § Relay identity](../contracts/SERVICE_ENDPOINTS.md#relay-identity))

## Relay links

A relay's address is its origin and its **relay id** (release-compatibility
[SIGNED_FORMATS § 3](../../projects/release-compatibility/SIGNED_FORMATS.md#3-the-relay-operator-set-and-certificate),
amending multi-relay MR008, where it was the PeerId): `relay:` + base64url-unpadded of BLAKE2b-256 of the relay's
founding operator set, 49 characters, as an Account ID is `account:` + the same encoding of its key's hash. Its
written form, the **relay link**, is

```
https://host[:port][/api/relay]#relay:<43 base64url characters>
```

(`ParseRelayLink`, `RelayLink`, `lib/net/certificate/RelayOrigins.h`; `RelayId`, `foundation/crypto/RelayId.h`). The fragment never
reaches the relay. A fragment that is not a relay id's one spelling — a PeerId (`#Qm…`, a link from before the
cutover) included — is refused (`relay_origin_bad_relay_id`); no link names a PeerId. The same `{origin, relay_id}`
is a contact locator's `relay` entry and, from card v5, an account card's relay entry. The PeerId a client signs for is
the one the relay's certificate names, never one a link or an answer gives.

## Derivation pipeline (frozen)

1. libp2p's `PublicKey { KeyType Type = 1; bytes Data = 2; }`, encoded by hand: `08 04` (key type 4 —
   ML-DSA-65; our assignment, libp2p defines 0–3), `12 A0 0F` (the key's length, 1952, as a varint),
   then the key — 1957 bytes.
2. An encoding over 42 bytes is hashed: multihash `12 20 ‖ SHA-256(encoding)` (libp2p inlines shorter
   ones; ML-DSA keys never are).
3. Base58btc of the multihash, no multibase prefix → `Qm…` for every ML-DSA key.

"libp2p-compatible" means the shape and string form; no libp2p network knows key type 4. PeerIds are
on the wire and stored everywhere (links, DHT records, the identity store, peer announces, call
control), so the format does not change.

## Public API

| Where | Symbol |
|-------|--------|
| pp-cpp-crypto `crypto/PeerId.h` | `pp::PeerIdFromMlDsaPublicKey()` |
| pp-cpp-amp `amp/link/PeerId.h` | `pp::amp::PeerIdFromIdentityKey` (for `PeerLinkConfig::peer_id_from_identity`), `pp::amp::PeerIdFromMultiaddr` |
| pp-browser `common/MultiaddrPeerId.h` | `PeerIdFromMultiaddr` for product and config code (foundation does not depend on pp-cpp-amp) |

## Golden vectors

pp-cpp-crypto's `tests/peer_id_test.cpp` gates byte-compat before any identity change:

| Key fixture | Expected PeerId |
|-------------|-----------------|
| 1952 × `0x42` | `QmTWkSQAcGsETogTFKrPJ3GzdHG3vP9UwXVyp2HeK9YBvP` |
| 1952 × `0x01` | `QmeKYz9h9AozszFqAkRzwxx4xGheEMYRVrLHuh7EMG7Bie` |

## MeshHost wiring

`MakeProductAmpLinkConfig` sets `peer_id_from_identity = pp::amp::PeerIdFromIdentityKey`, so links
name peers by the same PeerId product code derives.

## History

The vendored cpp-libp2p fork (`src/lib/libp2p/`) retained only PeerId + key wire (A017). It was
absorbed into `base/mesh/identity`, then `foundation/identity`, and moved to pp-cpp-crypto with the
mesh's extraction ([MESH.md](MESH.md)).
