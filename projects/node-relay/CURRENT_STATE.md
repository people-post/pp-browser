# Node relay — current state

**As of:** 2026-11-27

Design written ([DESIGN.md](DESIGN.md)). NR001–NR005 accepted (Hongwei: the edition on 2026-11-25; the protocol's
frames, a node relay as a relay, `amp_relays` and certificate version 2 on 2026-11-27). NR006–NR008 proposed. No code.

**Open, with defaults until decided:**
- Where a send goes (NR006): the first `https://` relay and also the first healthy Amp relay.
- The limits (NR007): 7 days; 1,000 records and 16 MiB per account.
- The registration policy's default (NR008): `self`.
- Where `relay-services` lives in the tree: `capabilities/relay` beside the others, gated like them.
- What an operator may log, for the privacy statement.

**Order with capability-repos:** pp-specs (its CR025 step 2) first, because the protocol and vectors live there; the
edition is built in this repository, where pp-node is; capability-repos' P4 moves pp-node, with the edition, once the
mailbox works.

**Next:** NR006–NR008 decided; then NR1 with capability-repos' pp-specs step.
