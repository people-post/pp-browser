# Profile model

What the app keeps on a device, and for whom: a **network** (main, staging, dev) on top, one **account** in it, this
install's **device profile** of that account, and inside it the state kept per **backend** (a group of services) and
per **relay**. The tester-app guard sits at the top: a tester app never opens a main profile.

Started from [#733](https://github.com/people-post/pp-browser/issues/733) (a reset profile loses its backend; a tester /
store build pair on one install can then fail to start). Pre-release: the layout changes as a clean cutover
([RC017](../release-compatibility/DECISIONS.md#rc017--p0s-decisions-a-clean-local-start-the-currency-id-fixed-two-features-after-release-1)).

Start with [DESIGN.md](DESIGN.md); decisions in [DECISIONS.md](DECISIONS.md); order of work in [PHASES.md](PHASES.md);
status in [CURRENT_STATE.md](CURRENT_STATE.md); what is on disk today, item by item, in [INVENTORY.md](INVENTORY.md).
