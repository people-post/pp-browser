# Node message relay — current state

**As of:** 2026-11-26

Design drafted ([DESIGN.md](DESIGN.md)); NM001–NM006 proposed, not decided. No code.

**Open, with defaults until decided:**
- Where the service lives: pp-cpp-mesh (beside the other node services) or pp-node in this repository. Default:
  pp-cpp-mesh's `l4/` services, as circuit and media relay.
- Retention and caps (NM004): 7 days; 1,000 records and 16 MiB per account.
- Subscription length: 30 days, renewed like a registration.
- Mailboxes per account: at most 2.
- The default policy when an operator turns it on (NM006): `self`.
- What an operator may log, for the privacy statement.

**Next:** decide NM001–NM006 with Hongwei; then NM1.
