# The net capability (projects/capability-repos CR022): the Brief backend client — registration, directory, blobs,
# push; deployment-specific (CR008). Its src/ mirrors the tree it came from ("lib/net/…"). It uses people and the
# base below it.
pp_browser_capability(net)

add_subdirectory(src/lib/net)
add_subdirectory(src/domain/registration)
