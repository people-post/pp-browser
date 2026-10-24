# Social posts (public, decentralized)

**Status:** Design draft (2026-10-23) — post log + trust ladder; distribution and social actions sketched  
**Related:** [content-cas](../content-cas/) (public realm holds media), [peer-scoped-broadcast](../peer-scoped-broadcast/) (announce plane carries "new head" tips), [ACCOUNT_CARD.md](../../docs/contracts/ACCOUNT_CARD.md) (who signed), [name directory](../p2p-mesh/NAME_DIRECTORY_NORTH_STAR.md) (chain later), pp-ledger (anchors)

Public posts with no central server: each author keeps an append-only, signed, hash-chained log; any
relay or peer may carry it because readers check it themselves. Times are claimed by the author,
optionally witnessed by services, and — with pp-ledger — anchored so that history cannot be backdated.

| Doc | Role |
|-----|------|
| [DESIGN.md](DESIGN.md) | Post log format, trust ladder, distribution and social-action sketch |
| [DECISIONS.md](DECISIONS.md) | S001– locked choices |
| [PHASES.md](PHASES.md) | Delivery order |
| [CURRENT_STATE.md](CURRENT_STATE.md) | Progress |
