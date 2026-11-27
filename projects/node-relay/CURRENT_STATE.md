# Node relay — current state

**As of:** 2026-11-27

Design written ([DESIGN.md](DESIGN.md)). Accepted by Hongwei: NR001 (the edition, 2026-11-25); NR002–NR007, NR009 and
NR010 (2026-11-27: the protocol's frames, a node relay as a relay, `amp_relays`, certificate version 2, the Amp copy
under delivery `first`, configurable limits with defaults, the recipient's delivery choice, files apart). NR008
proposed. No code.

**Open, with defaults until decided:**
- The registration policy's default (NR008): `self`.
- The settings' names (NR007): under the node config's relay services, each with a `PP_NODE_…` override.
- Where `relay-services` lives in the tree: `capabilities/relay` beside the others, gated like them.
- What an operator may log, for the privacy statement.

**Order with capability-repos:** pp-specs (its CR025 step 2) first, because the protocol and vectors live there; the
edition is built in this repository, where pp-node is; capability-repos' P4 moves pp-node, with the edition, once the
mailbox works.

**Next:** NR008 decided; then NR1 with capability-repos' pp-specs step.
