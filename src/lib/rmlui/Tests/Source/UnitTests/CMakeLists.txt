set(TARGET_NAME "rmlui_unit_tests")

add_executable(${TARGET_NAME}
	Animation.cpp
	ClickRouting.cpp
	Core.cpp
	DataBinding.cpp
	DataExpression.cpp
	DataModel.cpp
	Debugger.cpp
	Decorator.cpp
	Element.cpp
	ElementBackgroundBorder.cpp
	ElementDocument.cpp
	ElementHandle.cpp
	ElementFormControlSelect.cpp
	ElementImage.cpp
	ElementStyle.cpp
	EventListener.cpp
	Filter.cpp
	FlexFormatting.cpp
	Layout.cpp
	Localization.cpp
	main.cpp
	Math.cpp
	MediaQuery.cpp
	Properties.cpp
	PropertySpecification.cpp
	Selectors.cpp
	Specificity_Basic.cpp
	Specificity_MediaQuery.cpp
	StableVector.cpp
	StringUtilities.cpp
	StyleSheetParser.cpp
	Template.cpp
	URL.cpp
	Variant.cpp
	XMLParser.cpp
)

set_common_target_options(${TARGET_NAME})

target_include_directories(${TARGET_NAME} PRIVATE "${PROJECT_SOURCE_DIR}/Source/Core")

target_link_libraries(${TARGET_NAME} PRIVATE
	rmlui_tests_common
	rmlui_core
	doctest::doctest
	trompeloeil::trompeloeil
)

if(NOT EMSCRIPTEN)
	doctest_discover_tests(${TARGET_NAME})
	add_test(NAME rmlui_unit_tests COMMAND ${TARGET_NAME})
endif()
