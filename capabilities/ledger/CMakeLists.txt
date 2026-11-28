# The ledger capability (capability-repos CR027, node-relay NR001): pp-node's ledger role, a node service. Both pp-node
# presets link it; the desktop app never does. Built only with pp-ledger (PP_BROWSER_WITH_LEDGER; not on mobile). Its
# src/ mirrors the tree ("domain/ledger/…"). It uses the base below it; it goes with pp-node's repo.
pp_browser_capability(ledger)

if(PP_BROWSER_WITH_LEDGER)
  add_subdirectory(src/domain/ledger)
endif()
