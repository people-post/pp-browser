# The relay capability (projects/node-relay NR001): the message relay, an add-on of pp-node-plus — the mailbox (NR3),
# later the relay protocol's lockers, cards and directory. Only pp-node-plus links add-ons; the default pp-node never
# does (src/app/node/CMakeLists.txt checks both). Its src/ mirrors the tree ("domain/relay/…"). It uses net, people
# and the base below them: beside node, not on it (it runs on the mesh, pp-cpp-mesh).
pp_browser_capability(relay)

add_subdirectory(src/domain/relay)
