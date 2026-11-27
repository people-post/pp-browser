# Node relay

Relays that anyone can run on a pp-node: the message relay (the mailbox first), the first add-on of **`pp-node-plus`**,
a second build of pp-node for every add-on service, while the default `pp-node` stays as small as it is (NR001). It speaks the same relay protocol
as the org's HTTPS relay, carried over Amp. The first step of reducing the org's central services to defaults rather
than dependencies; it takes up the `message_relay` track that
[p2p-mesh N017](../p2p-mesh/DECISIONS.md#n017--split-n4-media-sfu-first-message-relay-separate-pricing-later)
deferred.

Start with [DESIGN.md](DESIGN.md); order of work in [PHASES.md](PHASES.md); status in [CURRENT_STATE.md](CURRENT_STATE.md);
decisions in [DECISIONS.md](DECISIONS.md).
