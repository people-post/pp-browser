# Node relay — current state

**As of:** 2026-11-27

Design written ([DESIGN.md](DESIGN.md)); NR001–NR011 accepted by Hongwei (NR001 on 2026-11-25; the rest on
2026-11-27, admission and ownership over several rounds). No code.

**Open, with defaults until decided:**
- The settings' names (NR007): under the node config's relay services, each with a `PP_NODE_…` override.
- Where `relay-services` lives in the tree: `capabilities/relay` beside the others, gated like them.
- What an operator may log, for the privacy statement.

**Later, by design (NR011):** offers to anyone, payment through the pricing project, org accounts and delegation.

**Order with capability-repos:** pp-specs (its CR025 step 2) first, because the protocol and vectors live there; the
edition is built in this repository, where pp-node is; capability-repos' P4 moves pp-node, with the edition, once the
mailbox works.

**Next:** NR1 with capability-repos' pp-specs step.
