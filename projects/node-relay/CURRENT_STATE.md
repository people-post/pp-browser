# Node relay — current state

**As of:** 2026-11-27

Design written ([DESIGN.md](DESIGN.md)); NR001–NR011 accepted by Hongwei (NR001 on 2026-11-25; the rest on
2026-11-27, admission and ownership over several rounds). On 2026-11-27 NR001 became two tiers: `pp-node-relay` is
`pp-node-plus`, one build for every add-on service, and the ledger role stays in the core, off unless asked.

- **NR1:** the protocol is drafted in pp-specs `contracts/relay/` ([pp-specs #4](https://github.com/people-post/pp-specs/pull/4), draft, under review).
  Its vectors come with NR3 and NR4.
- **NR2:** the edition is in review. It adds `pp-node-plus`, built beside `pp-node` from the same sources, and its first
  add-on, the empty `relay` capability (`capabilities/relay/`, `pp_domain_relay`: `relay::RelayService`). Services are
  chosen at compile time (`cmake/PpNodeServices.cmake`; the two presets `node` and `plus`, any other set `custom`), and
  `/status` reports `services` and `dormant` (NR001). `/status`
  carries `edition`. CI's `node-editions` job runs on every PR that touches what the node is built from. It builds
  both editions headless, and the link allowlist and symbol checks keep every add-on out of `pp-node`. It also
  holds the default binary (14 MiB) and its image over `ubuntu:24.04` (18 MiB) to their budgets. The budgets were
  measured at 11.7 and 14.5 MiB on 2026-11-27. Last, it builds both images and smoke-tests them; nothing is pushed.
  [BUILD § Headless mesh node](../../docs/ops/BUILD.md#headless-mesh-node-pp-node).

**Open, with defaults until decided:**
- The settings' names (NR007): one section per service in the node's config (`node.relay.…`, its on/off switch among
  them; tier-free, NR001), each with a `PP_NODE_…` override. NR3 adds the relay's.
- Where the add-ons live in the tree: each its own capability beside the others, gated like them (`capabilities/relay`
  first, taken in NR2).
- What `pp-node-plus` shares with `pp-node`: its config (`/etc/pp-node`), data dir, user and ports, so switching a
  node to pp-node-plus keeps them (NR2's packaging). Each add-on's own files go under `<data dir>/<add-on>`
  (`<data dir>/relay` first).
- What an operator may log, for the privacy statement.

**Later, by design (NR011):** offers to anyone, payment through the pricing project, org accounts and delegation.

**Order with capability-repos:** pp-specs (its CR025 step 2) first, because the protocol and vectors live there; the
edition is built in this repository, where pp-node is; capability-repos' P4 moves pp-node, with the edition, once the
mailbox works.

**NR3 must also:** follow NR001's data-folder rules 2 and 4 for the relay (expiry on start, a schema version on its
store; an explicit `node.relay` ask in a build without it fails the start).

**Separately (core):** a single-instance lock on the data folder, so a second process on the same folder exits with a
clear error (NR001's "never at once"): [#805](https://github.com/people-post/pp-browser/pull/805), in review.

**Next:** NR3, the mailbox on `pp-node-plus`, once pp-specs #4 is accepted. NR3 must combine the relay protocol with
the ledger role's protocols in `MeshHost::SetAppProtocols`, which replaces the whole list.
