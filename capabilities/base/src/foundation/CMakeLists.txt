include(PpBrowserBase)

# The base's foundation bands (ordered): runtime → TLS → error → diagnostics → crypto → identity.
add_subdirectory(runtime)
add_subdirectory(tls)
add_subdirectory(error)
add_subdirectory(diagnostics)
add_subdirectory(crypto)
add_subdirectory(identity)
