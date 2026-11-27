# The relay capability (projects/node-relay NR001): the relay services pp-node-relay runs beside a node's own — the
# mailbox (NR3), later the push gateway and files. Only pp-node-relay links it; the default pp-node never does
# (src/app/node/CMakeLists.txt checks both). Its src/ mirrors the tree ("domain/relay/…"). It uses node, people, net and
# the base below them.
pp_browser_capability(relay)

add_subdirectory(src/domain/relay)
