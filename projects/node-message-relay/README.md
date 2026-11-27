# Node message relay

pp-nodes as lightweight message relays: an account picks one or two nodes as **node mailboxes**, lists them in its
signed account card, and its contacts leave sealed messages there as well as at its HTTP relays. A second
store-and-forward path that needs no central relay, beside the HTTP relays, never replacing them. It is the
`message_relay` track that [p2p-mesh N017](../p2p-mesh/DECISIONS.md#n017--split-n4-media-sfu-first-message-relay-separate-pricing-later)
split off and deferred.

Start with [DESIGN.md](DESIGN.md); order of work in [PHASES.md](PHASES.md); status in [CURRENT_STATE.md](CURRENT_STATE.md);
decisions in [DECISIONS.md](DECISIONS.md).
