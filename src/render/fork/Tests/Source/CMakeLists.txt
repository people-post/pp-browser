add_subdirectory("Common")

if(RMLUI_BENCHMARKS)
	add_subdirectory("Benchmarks")
endif()

add_subdirectory("UnitTests")

if(RMLUI_VISUAL_TESTS)
	add_subdirectory("VisualTests")
endif()
