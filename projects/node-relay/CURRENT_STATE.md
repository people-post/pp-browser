# Node relay — current state

**As of:** 2026-11-27

Design written ([DESIGN.md](DESIGN.md)); NR001–NR011 accepted by Hongwei (NR001 on 2026-11-25; the rest on
2026-11-27, admission and ownership over several rounds).

- **NR1:** the protocol is drafted in pp-specs `contracts/relay/` ([pp-specs #4](https://github.com/people-post/pp-specs/pull/4), draft, under review).
  Its vectors come with NR3 and NR4.
- **NR2:** the edition is in review. It adds the empty `relay` capability (`capabilities/relay/`, `pp_domain_relay`:
  `RelayServices`, with no services yet) and `pp-node-relay`, built beside `pp-node` from the same sources. `/status`
  carries `edition`. CI's `node-editions` job runs on every PR that touches what the node is built from. It builds
  both editions headless, and the link allowlist and symbol checks keep the relay services out of `pp-node`. It also
  holds the default binary (14 MiB) and its image over `ubuntu:24.04` (18 MiB) to their budgets. The budgets were
  measured at 11.7 and 14.5 MiB on 2026-11-27. Last, it builds both images and smoke-tests them; nothing is pushed.
  [BUILD § Headless mesh node](../../docs/ops/BUILD.md#headless-mesh-node-pp-node).

**Open, with defaults until decided:**
- The settings' names (NR007): under the node config's relay services, each with a `PP_NODE_…` override.
- Where `relay-services` lives in the tree: `capabilities/relay` beside the others, gated like them (taken in NR2;
  the folder and gate name is `relay`, since the gates' names cannot hold a hyphen).
- What `pp-node-relay` shares with `pp-node`: its config (`/etc/pp-node`), data dir, user and ports, so switching a
  node to the relay edition keeps them (NR2's packaging). Its services' own files go under `<data dir>/relay`.
- What an operator may log, for the privacy statement.

**Later, by design (NR011):** offers to anyone, payment through the pricing project, org accounts and delegation.

**Order with capability-repos:** pp-specs (its CR025 step 2) first, because the protocol and vectors live there; the
edition is built in this repository, where pp-node is; capability-repos' P4 moves pp-node, with the edition, once the
mailbox works.

**Next:** NR3, the mailbox on `pp-node-relay`, once pp-specs #4 is accepted. NR3 must combine the relay protocol with
the ledger role's protocols in `MeshHost::SetAppProtocols`, which replaces the whole list.
