# Hard lab — current state

**Last updated:** 2026-09-03

## Direction

Design-complete hard lab for forced-hop / discovery / impairment simulation. **No compose or `--suite hard` yet.** Carry out from [PHASES.md](PHASES.md) starting at **h1**.

## Landed

| Area | State |
|------|-------|
| Project docs | README, DESIGN, DECISIONS, PHASES, CURRENT_STATE |
| Ladder / topology / profiles | [`packaging/pp-node/HARD_LAB.md`](../../packaging/pp-node/HARD_LAB.md) |
| Ops purpose rows + Gate F | [TEST_STRATEGY.md](../../docs/ops/TEST_STRATEGY.md) |
| Cross-links | IMAGE_SMOKE, TESTING, docs README, projects index |

## Gaps

| Area | State |
|------|-------|
| `docker-compose.hard-lab.yml` | Not started |
| Scenario scripts / `--suite hard` | Not started |
| Wave 1+ inventory evidence | Design only |

## Next

1. **h1** — forced-hop compose + `N-HARD-FORCE` green on clean links  
2. Then `B-HARD-CALL` / `B-HARD-MSG+CALL`  
3. Do not start Wave 4 until media-hop L3.5 exists  
