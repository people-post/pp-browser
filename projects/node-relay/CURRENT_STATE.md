# Node relay — current state

**As of:** 2026-11-27

Design written ([DESIGN.md](DESIGN.md)); NR001–NR011 accepted by Hongwei (NR001 on 2026-11-25; the rest on
2026-11-27, admission and ownership over several rounds). On 2026-11-27 NR001 became two tiers: `pp-node-relay` is
`pp-node-plus`, one build for every add-on service, and the ledger role stays in the core, off unless asked.

- **NR1:** the protocol is drafted in pp-specs `contracts/relay/` ([pp-specs #4](https://github.com/people-post/pp-specs/pull/4),
  ready, waiting for review). Its vectors come with NR3 and NR4.
- **NR2 done** (#804, merged 2026-11-27). A node's services are chosen at compile time
  ([`cmake/PpNodeServices.cmake`](../../cmake/PpNodeServices.cmake)). `pp-node` (preset `node`: `ledger`) and
  `pp-node-plus` (preset `plus`: `ledger`, `relay`) are the two we build; any other set reports `custom`. The relay
  capability (`capabilities/relay/`, `relay::RelayService`) is empty. `/status` carries `edition`, `services` and
  `dormant`. CI's `node-editions` job builds both presets headless and the default node with no service and with each
  alone. It holds the default binary (14 MiB) and its image over `ubuntu:24.04` (18 MiB) to their budgets, measured at
  11.7 and 14.5 MiB on 2026-11-27, and it smoke-tests both images (not pushed).
  [BUILD § Headless mesh node](../../docs/ops/BUILD.md#headless-mesh-node-pp-node).
- **One node per data folder** (#805, merged 2026-11-27): a node holds `<data dir>/pp-node.lock` for its life, so a
  second process on the folder exits (NR001's "never at once").

**Open, with defaults until decided:**
- The settings' names (NR007): one section per service in the node's config (`node.relay.…`, its on/off switch among
  them; tier-free, NR001), each with a `PP_NODE_…` override. NR3 adds the relay's.
- What `pp-node-plus` shares with `pp-node`: its config (`/etc/pp-node`), data dir, user and ports, so switching a
  node to pp-node-plus keeps them (NR2's packaging). Each add-on's own files go under `<data dir>/<add-on>`
  (`<data dir>/relay` first).
- What an operator may log, for the privacy statement.

**Later, by design (NR011):** offers to anyone, payment through the pricing project, org accounts and delegation.

**Order with capability-repos:** pp-specs (its CR025 step 2) first, because the protocol and vectors live there; the
edition is built in this repository, where pp-node is; capability-repos' P4 moves pp-node into pp-cpp-node (CR027, CR028), with the edition, once the
mailbox works.

**NR3 must also:** follow NR001's data-folder rules 2 and 4 for the relay (expiry on start, a schema version on its
store; an explicit `node.relay` ask in a build without it fails the start).

**Next:** NR3, the mailbox on `pp-node-plus`, once pp-specs #4 is accepted. NR3 must combine the relay protocol with
the ledger role's protocols in `MeshHost::SetAppProtocols`, which replaces the whole list.
