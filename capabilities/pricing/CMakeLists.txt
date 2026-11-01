# The pricing capability (projects/capability-repos CR020, CR022): initiation pricing and payment promises — what
# contacting someone costs, for messages and calls alike. Its src/ mirrors the tree it came from ("lib/pricing/…").
# It uses delivery and what is below it.
pp_browser_capability(pricing)

# Its stores keep data in the profile's SQLite, which headless configures do not build.
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(src/lib/pricing)
endif()
