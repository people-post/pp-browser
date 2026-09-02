include(PpBrowserBase)

# Foundation bands (ordered): runtime + platform → error/i18n; data/crypto follow later.
add_subdirectory(runtime)
add_subdirectory(platform)
add_subdirectory(i18n)
add_subdirectory(error)
