# Node relay — design

**Status:** Direction accepted (Hongwei, 2026-11-25 and 2026-11-27: NR001–NR005); NR006–NR008 proposed. No code.  
**Date:** 2026-11-27  
**Related:** [p2p-mesh N017](../p2p-mesh/DECISIONS.md#n017--split-n4-media-sfu-first-message-relay-separate-pricing-later),
[N032](../p2p-mesh/DECISIONS.md#n032--node-versions-claims-only-exclude-verification-promotes),
[N033](../p2p-mesh/DECISIONS.md#n033--nodes-say-how-busy-they-are-per-service-clients-act-on-the-level),
[multi-relay](../multi-relay/DESIGN.md), [release-compatibility SIGNED_FORMATS](../release-compatibility/SIGNED_FORMATS.md)
(§ 2.8 signed calls, § 3 the relay certificate), [capability-repos](../capability-repos/DESIGN.md) (CR025 pp-specs, P4),
[L4_PROTOCOL_KINDS](../../docs/contracts/L4_PROTOCOL_KINDS.md)

## Why

The app keeps working for a group whose only relay is one of them: the org's services become defaults, not
dependencies. Mail is the first service, because an offline message always waits on a relay today, and today every
relay is an HTTPS service on a server.

## Today

A message goes directly over Amp when both ends are online (`/pp/rpc/chat/1.0.0`). Otherwise it goes, sealed, to the
recipient's relays in the order of its card's `relays`, **to the first that takes it**
([SERVICE_ENDPOINTS § Sending by the list](../../docs/contracts/SERVICE_ENDPOINTS.md#sending-by-the-list)). A
contact's send carries the recipient's grant; a stranger's pays the recipient's floor or uses a small free quota. Each
device polls each relay with its own cursor, and the relay's push wakes a phone. A relay is an operator set with a
relay id and a certificate binding its transport key and its `https://` origins (SIGNED_FORMATS § 3). A message that
arrives twice is dropped by its id (`RelayReceivePipeline`, `BenignDuplicate`).

## The shape

```
                         the relay protocol (pp-specs): each call's signed bytes, objects, refusal codes
                         ├── HTTPS binding (today, unchanged) ── the org's relay on AWS; every app; background work
                         └── Amp binding  /pp/rpc/relay/1.0.0 ── pp-node-relay (and later the org's relay too)

 pp-node (default)  = mesh, DHT, directory, circuits, media hop                      (unchanged, stays small)
 pp-node-relay      = the same  + relay-services (mailbox first; lockers, cards, files later) + a small store
```

### 1. A second build, not a plugin (NR001)

`pp-node-relay` is built from the same commit, at the same version (pp-node's line, RC016), with its own image. It adds
the `relay-services` capability; the default `pp-node` does not link it, and CI checks both that and the default
image's size. No runtime plugin (C++ has no stable binary interface) and no separate repo (pin skew, hook drift, release
lag): the edition lives where pp-node lives, in this repository today and in pp-node's own once capability-repos' P4
moves it.

### 2. One protocol, two bindings (NR002)

The relay's calls are already defined by their signed bytes, with the relay's transport PeerId as the audience
(SIGNED_FORMATS § 2.8):

```
"pp-browser:relay-api" 00 ‖ u8 4 ‖ LenUtf8 audience ‖ u8 op ‖ i64 timestamp ‖ …the op's fields… ‖ ext area
```

pp-specs states each call once, apart from any transport: its signed bytes, request and response objects, refusal
codes and vectors. The **HTTPS binding** is today's. The **Amp binding** is `/pp/rpc/relay/1.0.0`, of the `rpc` kind:

| Frame | Shape |
|-------|-------|
| Request | `{op, id, body}`: `body` is the HTTPS JSON body, byte for byte (`sign_version`, `audience`, the fields, `signature`) |
| Response | `{id, status, code?, body}`: HTTPS's status numbers and refusal codes, one table for both bindings |
| `watch` | a held exchange; the relay sends `{op: "mail"}` when mail arrives for the watching account, so a connected device stops polling |

Signatures stay over Amp, although the handshake proves both peers: records are stored and forwarded, and one check
serves both bindings.

### 3. A node relay is a relay (NR003)

It has its own operator set (1 of 1 for a person's node), relay id and certificate, made with the operator tools
([pp-ledger-apps operator app](https://github.com/people-post/pp-ledger-apps/blob/main/projects/operator-app/DESIGN.md)
or the relay's tool). Registration, grants, the stranger rules, polling with a cursor and `clear` are the relay
protocol's, unchanged. Its operator decides whom it registers (NR008).

### 4. Listing and reaching a relay that has no `https://` address (NR004, NR005)

- **The card:** `relays` entries must be `https://` URLs, and a card breaking that bound is refused whole by apps and
  relays (MR004). So Amp relays go in a **non-critical extension entry**, `amp_relays: [{relay_id, hints}]`, beside
  `relays`. Apps and relays before it skip it (NR004).
- **The certificate:** version 1 needs 1–4 `https://` origins. **Version 2** may have none when it declares the Amp
  binding; its bindings are a signed field. Only apps that read `amp_relays` ever fetch one, so no older reader meets
  it (NR005).
- **Dialling:** relay id → its certificate (asked of the relay over Amp, `relay-identity`, checked against the relay
  id, the highest kept as today) → the transport key's PeerId → an address from the hints, the mesh directory or the
  DHT. The Amp handshake proves the PeerId the certificate names.

### 5. Where a send goes (NR006)

A send goes to the first `https://` relay that takes it, as today, **and also** to the first healthy Amp relay of
`amp_relays`. The `https://` copy keeps phones waking (a node cannot push, until the push gateway) and keeps mail when a
home node is off; the second copy is dropped by id. A failed Amp deposit never fails the send. Once the push gateway
exists, an account may list Amp relays only.

### 6. Light (NR007)

Records expire after 7 days (a sender's shorter `ttl_s` tier sooner); at most 1,000 records and 16 MiB per account; the
node's total is its operator's budget (`mesh.capacity`); a full relay answers `busy` and its load level says so (N033).
The relay's other limits (quotas, sizes) are the protocol's.

### 7. Phones: the push gateway (later)

The app registers its APNs or FCM token with the org's **push gateway** and gets an opaque, revocable **wake token** per
relay, which it gives the relay at registration. The relay calls the gateway with a signed `wake(token)`; the gateway
sends a push with no content. The gateway learns a device, a relay and a time, never a message or its sender. Until it
exists, phones wake on the `https://` copy (NR006).

## What a node relay learns

What any relay learns: the recipient's account, the sender's (from the grant or its signature), sizes, times, and the
sender's address on the Amp link; never content, which is sealed end to end. It is a relay the recipient chose to list.
The privacy project states it before the card entry ships, with what an operator may log.

## Compatibility

All additive under the range rule: a non-critical card entry, a certificate version only `amp_relays` readers fetch, a
new `rpc` protocol, the edition's feature claim (`relay` in N032's `features`). The HTTPS binding and its relays are
unchanged.

## Not in this project

Replacing the org's HTTPS relay; the Amp binding on the org's relay (it runs on serverless functions; later, if ever);
lockers, cards, files and directory on node relays (after the mailbox, the same way); paying a relay (pricing); node-only
accounts before the push gateway.
