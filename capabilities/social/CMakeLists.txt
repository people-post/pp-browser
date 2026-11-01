# The social capability (projects/capability-repos CR022): public posts and the content store they share with
# attachments (CR009); never above messaging. Its src/ mirrors the tree it came from ("lib/posts/…"). It uses people
# and the base below it.
pp_browser_capability(social)

# Both keep their data in the profile's SQLite, which headless configures do not build.
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(src/lib/posts)
  add_subdirectory(src/lib/content)
endif()
