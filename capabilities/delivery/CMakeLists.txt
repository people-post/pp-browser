# The delivery capability (projects/capability-repos CR020, CR022): pairwise and public E2E sessions, envelopes and
# their codecs, the control lane, the outbox, the relay's message client, and the records they carry (threads,
# messages, channels) with the store ports. Messaging and realtime both ride it. Its src/ mirrors the tree it came
# from ("lib/delivery/…", "common/chat/…"). It uses people, net and the base below them.
pp_browser_capability(delivery)

# Its common layer: the records and their DTOs (chat payloads, relay envelopes, the thread store ports).
add_library(pp_delivery_common STATIC
  src/common/chat/ChatPayloadTypes.cpp
  src/common/chat/MessagingJson.cpp
  src/common/chat/RelayStreamKey.cpp
  src/common/chat/SystemControl.cpp)
target_include_directories(pp_delivery_common PUBLIC ${PP_BROWSER_CAPABILITY_SRC})
target_link_libraries(pp_delivery_common PUBLIC pp_pbr_common)
pp_browser_apply_warnings(pp_delivery_common)
if(PP_BROWSER_BUILD_TESTS)
  add_subdirectory(src/common/chat/tests)
endif()

# Sessions, envelopes and the relay client keep state in the profile's SQLite, which headless configures do not build.
if(NOT PP_BROWSER_HEADLESS)
  add_subdirectory(src/lib/delivery)
  add_subdirectory(src/domain/delivery)
endif()
