# The node capability (projects/capability-repos CR008, CR022): node services and roles — what a node hosts, its DHT
# and directory config, its own directory row, and the ledger role. Headless: pp-node and the product's mesh share it.
# Its src/ mirrors the tree it came from ("domain/node/…"). It uses people, net and the base below them.
pp_browser_capability(node)

add_subdirectory(src/domain/node)
