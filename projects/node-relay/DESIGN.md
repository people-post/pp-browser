# Node relay — design

**Status:** Accepted (Hongwei, 2026-11-25 and 2026-11-27): NR001–NR011. No code.  
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
protocol's, unchanged. Who may register is by invite (below, NR008).

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

### 5. Where a send goes (NR009, NR006)

The recipient chooses, in its card (NR009):

| `delivery` | A contact's send goes to | Push |
|------------|--------------------------|------|
| `first` (default) | the first `https://` relay that takes it, as today, and also the first healthy Amp relay (NR006) | the copy at the `https://` relay |
| `all` | every relay of `relays` and of `amp_relays` | only the first copy accepted carries the push hints; the rest are stored silently |

`first` suits a recipient whose relays are all reachable to it; `all` suits one whose access changes (some hosts blocked
at times, UDP blocked at work or in hotels, a home node unreachable from outside), since any one reachable relay then
delivers. A stranger's send always stops at the first relay. A failed Amp deposit never fails a send. Duplicates are
dropped by message id. Once the push gateway exists, an account may list Amp relays only.

### 6. Light, and configurable (NR007)

Every mailbox limit is a setting with a default: records kept 7 days (a sender's shorter `ttl_s` tier sooner); 1,000
records and 16 MiB per account; a node-wide budget from `mesh.capacity`. A full relay answers `busy` and its load level
says so (N033). The relay reports its limits in `relay-identity`, so a recipient's app can show how long its relay keeps
mail.

### 7. Files (NR010)

The mailbox carries messages only, each within the sealed payload's 128 KiB; files travel apart, as today: directly
between devices (`/pp/blob/1.0.0`) or through a relay's file store, with the message carrying a reference. The mailbox's
limits count messages only, and `all` multiplies messages, not files. On node relays a `files` service, with limits of
its own, comes after the mailbox. Until then an attachment's link names the sender's relay: a recipient who cannot reach
it fetches from the sender's devices when they are online.

### 8. Who may register: invites, owners and offers (NR008, NR011)

A node relay admits an account with an **invite** signed with its owner's authority; the owner's own account needs
none. Friends get one from the owner's app ("Invite to my relay", on their contact page). Invites last 90 days and are
renewed with 30 left by the sweep that renews relay grants; only the owner's app can renew them. Removing or blocking
the friend revokes at once (a signed revoke to the owner's node). Then the relay takes no new mail for them, and mail
already waiting stays fetchable until it expires; "Remove and delete their mail now" is there for abuse.

Ownership is proven both ways: the owner's card lists the relay, the relay's certificate names the owner. The owner's
**offer** says who may ask, the limits and a price in `pp_credit` (0 for now). Later, paying (a payment promise naming
the offer) and an org's delegates are just other ways to get the same invite. Every record names its provider by id and
is checked as signed with the provider's authority, with room for an org's key set and delegation, so org accounts need
no format change.

### 9. Phones: the push gateway (later)

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
lockers, cards and directory on node relays (after the mailbox, the same way; files are NR010's later service); paying a relay (pricing); node-only
accounts before the push gateway.
