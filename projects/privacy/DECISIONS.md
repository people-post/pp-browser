# Privacy — decisions

## P001 — The unit is address disclosure

**Date:** 2026-09-30
**Decision:** The T1 control gates *every way our IP reaches a peer* — our dials and punches, answering a punch as target, listen addresses in call signalling and ch0, published records — not only "direct connections" chosen by path logic.
**Rationale:** Gating only direct calls leaves the IP one punch request or directory lookup away: the punch target answers any initiator, ch0 hands every linked peer our full address list including LAN IPs, and client registration publishes addresses.

## P002 — Default audience: contacts

**Date:** 2026-09-30
**Decision:** `mesh.direct_connections` defaults to `contacts`; `friendly` and `nobody` are the stricter choices, `everyone` the looser. Blocked peers are never allowed.
**Rationale:** Strangers are the main threat, and a saved contact is a deliberate act. `friendly` as the default would relay nearly every call and chat (few contacts are marked), costing call quality and relay capacity on pp-node operators. Precedent: Signal relays calls from non-contacts; Telegram offers everyone / contacts / nobody for peer-to-peer calls.

## P003 — Existing links are used

**Date:** 2026-09-30
**Decision:** The audience gates new disclosure (dial, punch, address sharing). A link that already exists — including one a stranger opened to us — may carry traffic.
**Rationale:** The peer on an existing link already knows the address; refusing to use the link hides nothing. Refusing strangers' links at all is T3 (inbound control), not T1.

## P004 — Nodes are public by role

**Date:** 2026-09-30
**Decision:** In the Node role (pp-node, desktop Node) the audience is `everyone`.
**Rationale:** A Node serves relaying, DHT and directory to anyone; its address is published on purpose. Operators who need privacy do not run a Node.

## P005 — Watching a stranger's broadcast is the viewer's choice

**Date:** 2026-09-30
**Decision:** Peer-announce fetches and broadcast viewing reach the publisher (and its hops) the viewer chose to watch without the direct-audience gate; they use the plain links.
**Rationale:** Opening a stranger's program is a deliberate act toward that publisher, like visiting a website: the viewer accepts that the publisher's side sees its address. Gating it would make every stranger's broadcast unwatchable. The audience still governs everything the viewer did not choose (inbound punches, calls, chat, signalling).

## P006 — Blocked means no contact at all

**Date:** 2026-09-30
**Decision:** A Blocked contact's direct messages and call control are discarded on receipt (on the claimed sender, before decryption), and its Amp link is dropped as soon as it connects. Group messages from a Blocked member are not filtered yet.
**Rationale:** Blocked used to shape only our own routing, payments and group invites — a blocked person could still message and ring. Dropping on the claimed sender is safe: spoofing a Blocked id only discards the spoofer's own message. Group traffic carries roster / epoch control from every member; filtering it needs its own design.

## P007 — Who can call me; strangers' messages stay open for now

**Date:** 2026-09-30
**Decision:** `call_invite_policy` (preferences: `everyone` default | `contacts_only` | `nobody`; Me → Security) drops invites from outside the audience silently — no decline, the caller learns nothing. Messages from strangers are not restricted yet.
**Rationale:** Calls from strangers already run relay-only (T1), so the default stays reachable; the setting is for users who want quiet. Restricting stranger messages without a "message requests" inbox would silently lose first contacts (directory discovery depends on them) — that needs UI, and is left open.

## P008 — Trusted relays: org seeds and Friendly contacts' nodes

**Date:** 2026-09-30
**Decision:** `mesh.trusted_relays_only` (off by default; Me → Network) limits every relay role — rendezvous parking, circuit dialing, punch introducers, call media hops — to the configured org seeds and Friendly contacts' nodes. Directory volunteers and DHT-discovered nodes are left out, and the bootstrap set drops the directory nodes merged into it.
**Rationale:** A relay operator sees both ends' addresses and who talks to whom. The org already sees routing metadata (T2), and a Friendly contact is someone the user chose to trust; a volunteer node is neither. Off by default because the narrower set can make connections slower or fail when the org seeds are loaded or unreachable.
