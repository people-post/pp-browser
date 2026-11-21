# Task scopes

Every posted task belongs to a **task scope** tied to its owner's lifetime. The scope holds the owner's own
sequence (a serial lane on the shared pool, the UI thread or an owner thread), counts its pending work, and is
closed on that sequence before the owner's members die. Queued tasks then do nothing; running ones finish. Owners
stop posting closures that capture raw `this` onto the process-wide mailboxes, and teardown, profile reset and
mid-life restarts become owner by owner. The global teardown quiesce stays only as a process-exit backstop. No
wire or disk format changes. The project is planned for after release 1, because it touches core threading.

Start with [DESIGN.md](DESIGN.md). The order of work is in [PHASES.md](PHASES.md), status in
[CURRENT_STATE.md](CURRENT_STATE.md), and decisions in [DECISIONS.md](DECISIONS.md) (TS001, with the open questions
for Hongwei). Today's model: [THREADING.md](../../docs/architecture/THREADING.md),
[OWNERSHIP.md](../../docs/architecture/OWNERSHIP.md).
