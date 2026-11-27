# The relay capability (projects/node-relay NR001): the message relay, an add-on of pp-node-plus — the mailbox (NR3),
# later its push gateway. Only pp-node-plus links add-ons; the default pp-node never does (src/app/node/CMakeLists.txt
# checks both). Its src/ mirrors the tree ("domain/relay/…"). It uses node, people, net and the base below them.
pp_browser_capability(relay)

add_subdirectory(src/domain/relay)
