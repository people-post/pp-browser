# Thread coordinator and worker pool

**Status:** **t0 design** — phase 1 next  
**Normative design:** [`docs/architecture/THREADING.md`](../../docs/architecture/THREADING.md)

Consolidate pp-browser threading from `BrowserThread::IO` + ~25 detached hop-offs into a **coordinator mailbox** (with **timer wheel**) and a **bounded priority worker pool**.

## Files

| File | Purpose |
|------|---------|
| [DESIGN.md](DESIGN.md) | Project scope and pointers |
| [PHASES.md](PHASES.md) | Implementation checklist — **delete this project when all phases ship** |
| [CURRENT_STATE.md](CURRENT_STATE.md) | What exists in code today |

## Exit criteria (project archive)

- [ ] All phases in [PHASES.md](PHASES.md) complete
- [ ] [THREADING.md](../../docs/architecture/THREADING.md) “Today” section updated or removed; target is live code
- [ ] No `.detach()` hop-offs remain in `src/` (except documented third-party fork sites)
- [ ] `projects/thread-coordinator/` folder deleted
