# Node message relay — decisions

Rationale for [DESIGN.md](DESIGN.md). Outcomes move to `docs/` when they ship; mark the entry **superseded by** that
doc then.

## NM001 — Node mailboxes beside the HTTP relays, never instead

**Date:** 2026-11-26  
**Status:** Proposed  
**Decision:** A sender writes to the recipient's HTTP relays as today and also deposits at its node mailboxes. The HTTP
relay stays the durability anchor and the source of push wake-ups. A deposit failure never fails a send.  
**Rationale:** [N017](../p2p-mesh/DECISIONS.md#n017--split-n4-media-sfu-first-message-relay-separate-pricing-later):
offline delivery needs durability and wake-ups a volunteer node cannot promise, and peer relays "may never be the
default". Writing to both adds a path that needs no central relay without making anything depend on a node.  
**Alternatives:** Node first, HTTP as fallback (a slow or dead node delays every message); node only (no phone wake;
out of scope).

## NM002 — Contacts only: a deposit carries the recipient's grant

**Date:** 2026-11-26  
**Status:** Proposed  
**Decision:** A node accepts a deposit only with a valid grant from the subscribed recipient to this sender. Strangers
keep using the HTTP relay's price and free quota.  
**Rationale:** A node has no payment path, and a free mailbox without a grant is a spam sink paid for by a volunteer.
Grants exist already ([multi-relay MR005](../multi-relay/DECISIONS.md#mr005--strangers-pay-the-receivers-price-contacts-carry-a-grant)),
and the node can check them with the recipient's key from the subscription.  
**Alternatives:** A stranger quota on nodes too (abuse falls on volunteers); a node price (pricing is later, N017).

## NM003 — The recipient chooses; its card lists the mailboxes

**Date:** 2026-11-26  
**Status:** Proposed  
**Decision:** An account picks up to two node mailboxes. Its signed account card lists them by PeerId (with an address
hint) in a non-critical extension-area entry, beside `relays`.  
**Rationale:** The recipient bears what its mailbox learns, so it chooses whom to trust, as with its HTTP relays. The
signed card is how senders already learn where to send; a non-critical entry is additive under the range rule. The Amp
handshake proves a PeerId, so a node needs no certificate.  
**Alternatives:** Senders pick any node claiming the feature (the recipient would not know who holds its mail); the
directory chooses (a central choice again).

## NM004 — Light: bounded, short-lived, best effort

**Date:** 2026-11-26  
**Status:** Proposed  
**Decision:** Records expire after 7 days (shorter for a sender's `ttl_s` tier); at most 1,000 records and 16 MiB per
account; a node-wide budget from its operator's `mesh.capacity`; `busy` when full, with the load level (N033). A node
may lose records (a restart, a full disk) without harm.  
**Rationale:** The HTTP copy carries durability (NM001), so a node can stay small enough for a Raspberry Pi and cheap
enough to volunteer.  
**Alternatives:** HTTP-like retention, 90 days (a volunteer's disk and liability for little gain).

## NM005 — One `rpc` protocol: subscribe, deposit, fetch, ack, clear

**Date:** 2026-11-26  
**Status:** Proposed  
**Decision:** `/pp-browser/rpc/mailbox/1.0.0`, of the `rpc` kind ([A028](../adp/DECISIONS.md#a028--l4-protocol-kinds--seven-conversation-shapes)).
Records keep the HTTP relay's shape; requests are signed under their own domains, with an extension area, per the
signed-format rules.  
**Rationale:** A028 makes message relay an `rpc` host role, not a new kind. One record shape lets the app merge node and
HTTP inboxes with the code it has.  
**Alternatives:** Over HTTP on the node (a second server on every node, reachability again); blob kind (records are
small and many).

## NM006 — The operator decides whom a node serves

**Date:** 2026-11-26  
**Status:** Proposed  
**Decision:** pp-node's `capabilities.message_relay` (`PP_NODE_CAP_MESSAGE_RELAY`; the key p2p-mesh's design already reserves; off by default) and a subscription policy: `self` (the node's own
account and its devices), `allowlist` (named accounts), or `open` (anyone, within the node's budget). Default when on:
`self`.  
**Rationale:** The first users are people running their own node (desktop, Pi) for themselves and their family;
opening to strangers' subscriptions is a later, deliberate step.  
**Alternatives:** Open by default (every volunteer node becomes everyone's mailbox at once).
