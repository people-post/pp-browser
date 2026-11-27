# Node message relay — design

**Status:** Proposed (NM001–NM006). No code.  
**Date:** 2026-11-26  
**Related:** [p2p-mesh N017](../p2p-mesh/DECISIONS.md#n017--split-n4-media-sfu-first-message-relay-separate-pricing-later)
(the `message_relay` track), [N032](../p2p-mesh/DECISIONS.md#n032--node-versions-claims-only-exclude-verification-promotes)
(what a node claims), [N033](../p2p-mesh/DECISIONS.md#n033--nodes-say-how-busy-they-are-per-service-clients-act-on-the-level)
(load levels), [multi-relay](../multi-relay/DESIGN.md) (the relay list, grants), [L4_PROTOCOL_KINDS](../../docs/contracts/L4_PROTOCOL_KINDS.md),
[release-compatibility](../release-compatibility/DESIGN.md) (the range rule)

## Today

A message is written locally first. When both ends are online it goes directly over Amp (`/pp-browser/rpc/chat/1.0.0`).
Otherwise it goes, sealed, to the recipient's HTTP relays in the order of its card's `relays` list
([SERVICE_ENDPOINTS § Sending by the list](../../docs/contracts/SERVICE_ENDPOINTS.md#sending-by-the-list)). A contact's
send carries the recipient's **grant**; a stranger's pays the recipient's floor or uses a small free quota
([ACCOUNT_CARD § Relay grants](../../docs/contracts/ACCOUNT_CARD.md#relay-grants)). The recipient's devices poll each
relay with their own cursor, and a push from the relay wakes a phone. The same message arriving twice is dropped by its
message id (`RelayReceivePipeline`, `BenignDuplicate`).

So an offline message always waits on an HTTP relay. A relay down, blocked or gone means mail waits until it is back.

## What a node mailbox adds

A **node mailbox** is a pp-node that holds sealed messages for an account until its devices fetch them. The account
chooses it: its own node (a desktop left on, a Raspberry Pi), a friend's, or a volunteer node. It is:

- **beside the HTTP relays, never instead** (NM001): senders write to both; the HTTP relay stays the durability anchor
  and the source of phone wake-ups (N017: peer relays "may never be the default");
- **for contacts** (NM002): a deposit carries the recipient's grant; strangers keep using the HTTP relay's price and
  quota;
- **light** (NM004): days, not forever; bounded per account and per node; losing a node's copy loses nothing, because
  the HTTP copy exists.

## How it works

```
 sender app                 recipient's node mailbox                recipient's devices
 ──────────                 ────────────────────────                ───────────────────
 HTTP relays by list  (unchanged)
 + deposit over Amp ─────►  check the grant; keep the
   (sealed record + grant)  record, bounded          ◄────────────  fetch over Amp (signed, a cursor
                                                                    per device), merge with the HTTP
                                                                    inbox, duplicates dropped by id
```

1. **Subscribe.** The recipient's main device asks the node to hold mail for its account: a signed request carrying
   its account card. The node keeps the card (the account's signing key, to check grants and fetches) for as long as
   the subscription lasts (default 30 days, renewed like a registration). The node may refuse: it is full, or it serves
   only accounts its operator allows (NM006).
2. **List.** The account's card lists its node mailboxes by PeerId, in a non-critical entry of the card's extension area
   (NM003). The Amp handshake proves the PeerId, so the card needs no certificate for a node; a node changing its key is
   a new entry. Older apps skip the entry.
3. **Deposit.** After the HTTP send by the list (unchanged), the sender also deposits the same sealed record at each
   listed node mailbox: dialled by PeerId (directory, DHT, or the address the card's entry carries), over the `rpc`
   kind (`/pp-browser/rpc/mailbox/1.0.0`, NM005), with the grant. The node checks the grant (the subscribed account's
   signature, this sender, not expired) and keeps the record in the HTTP relay's record shape (sender, `stream_id`,
   `index_key`, blob, `created_at`). A deposit failure never fails the send: the HTTP copy carries it.
4. **Fetch.** Each device fetches with a signed request and its own cursor, as it polls an HTTP relay. Ack is soft and
   `clear` deletes, as on HTTP ([SERVICE_ENDPOINTS § Inbox](../../docs/contracts/SERVICE_ENDPOINTS.md)). Records join
   the HTTP inbox's, and the second copy of a message is dropped by id.
5. **Bounds.** A record expires after 7 days (a sender's `ttl_s` tier shorter); an account holds at most 1,000 records
   and 16 MiB; the node's total is its operator's budget (`mesh.capacity`). A full node or account refuses with
   `busy`, and the node's load level says so (N033), so senders stop trying it for a while.

## What a node learns

The node never holds anything readable: records are sealed end to end before they leave the sender. It does learn the
recipient's account, the sender's account (from the grant), sizes and times, and the sender's address on the Amp link.
That is what an HTTP relay learns, but held by a node the recipient chose (and so trusts no less than its relay). It
must be stated in the privacy project before NM2 ships, with what an operator may log.

## Phones

A node cannot wake a phone: it holds no push credentials, and should not. A phone collects node mail when it is open,
or when the HTTP relay's push wakes it for the same message. So node mailboxes help most for desktops and always-on
devices, and as the path that still works when the HTTP relay does not. Node-only accounts (no HTTP relay at all) are
not in this project: they need a wake path first.

## Compatibility

Everything is additive: a non-critical card entry, a new `rpc` protocol, a node feature claim (`mailbox` in N032's
`features`). Apps and nodes before it ignore all three, so the range rule holds without a cutover. An app uses a node
mailbox only when the node claims the feature and the protocol opens (claims only exclude, N032).

## Not in this project

Replacing the HTTP relay; node-only accounts; strangers' mail on nodes; paying a node (pricing, later, without changing
the protocol); delivery over several hops; group mailboxes beyond what each member's own mailbox gets.
