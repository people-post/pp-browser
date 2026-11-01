# AI friend search and selection

User requirement (2026-10-27): asking the assistant to find or add a person by account/relay ID or nickname searches the directory and shows the existing people list. Only clicking a result's Add contact action saves the person. Existing contacts show their existing-contact actions; empty results say no person was found.

This narrows the old v1b PeerId mutation proposal. Raw mesh PeerId lookup/addition and the broader intent taxonomy are outside this change.

| Step | Acceptance | Status |
|------|------------|--------|
| Route find/add requests to directory search | Planner instructions retain the exact supplied ID/nickname; on-device add intent reaches the search capability | Implemented; automated routing checks passed; real-model check pending |
| Keep addition on result selection | Native AI tool catalog cannot add a contact; existing result action dispatch still saves the selected hit | Implemented; catalog test and dispatch review passed; device click pending |
| Render people results consistently | People-only discovery plans use the existing list, including empty and already-saved results | Implemented; list and plan tests passed |
| Regression checks and documentation | Targeted classifier/plan/list tests; compile touched production units; update current state and stable UI contract | 32 cases passed; changed units compiled; docs updated; independent review fix covered |

Device validation: pending. Automated checks do not establish live directory availability or a production app installation.
