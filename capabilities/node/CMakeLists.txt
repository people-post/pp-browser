# The node capability (projects/capability-repos CR008, CR022): what a node hosts, its DHT and directory config, its own
# directory row. Headless: pp-node and the product's mesh share it. The ledger role is its own capability
# (capabilities/ledger, CR027), a node service only pp-node links.
# Its src/ mirrors the tree it came from ("domain/node/…"). It uses people, net and the base below them.
pp_browser_capability(node)

add_subdirectory(src/domain/node)
