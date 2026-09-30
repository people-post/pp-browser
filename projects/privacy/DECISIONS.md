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
