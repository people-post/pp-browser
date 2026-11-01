# The messaging capability (projects/capability-repos CR008, CR020, CR022): chat on top of delivery — the stores,
# inbox, groups, attachments, history and the conversations hub. Its src/ mirrors the tree it came from
# ("lib/messaging/…", "feature/conversations/…"). It uses delivery, pricing, social and what is below them; its
# targets link only what they name (no pp_base / pp_feature bundles).
pp_browser_capability(messaging)
include(PpBrowserFeature)

# Chat keeps its data in the profile's SQLite, which headless configures do not build.
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(src/lib/messaging)
  add_subdirectory(src/domain/attachments)
  add_subdirectory(src/feature/conversations)
endif()
