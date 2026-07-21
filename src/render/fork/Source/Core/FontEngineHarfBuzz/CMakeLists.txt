# HarfBuzz-backed font engine (ported from upstream RmlUi Samples/basic/harfbuzz).
target_sources(rmlui_core PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}/FontEngineInterfaceHarfBuzz.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontEngineInterfaceHarfBuzz.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontFace.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontFace.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontFaceHandleHarfBuzz.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontFaceHandleHarfBuzz.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontFaceLayer.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontFaceLayer.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontFamily.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontFamily.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontGlyph.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontProvider.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/FontProvider.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/FreeTypeInterface.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/FreeTypeInterface.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/LanguageData.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/TextureLayout.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/TextureLayout.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/TextureLayoutRectangle.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/TextureLayoutRectangle.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/TextureLayoutRow.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/TextureLayoutRow.h"
	"${CMAKE_CURRENT_SOURCE_DIR}/TextureLayoutTexture.cpp"
	"${CMAKE_CURRENT_SOURCE_DIR}/TextureLayoutTexture.h"
)

target_include_directories(rmlui_core PRIVATE
	"${CMAKE_CURRENT_SOURCE_DIR}"
	"${CMAKE_CURRENT_SOURCE_DIR}/.."
)

target_compile_definitions(rmlui_core PRIVATE RMLUI_FONT_ENGINE_HARFBUZZ)
