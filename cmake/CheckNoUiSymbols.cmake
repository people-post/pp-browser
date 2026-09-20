# Invoked as:
#   cmake -DPP_BROWSER_NO_UI_BINARY=... -DPP_BROWSER_NO_UI_SYMTOOL=...
#         -DPP_BROWSER_NO_UI_SYMTOOL_KIND=nm|dumpbin -P CheckNoUiSymbols.cmake
#
# Legacy: PP_BROWSER_NO_UI_NM still accepted as nm.
if(NOT PP_BROWSER_NO_UI_BINARY OR NOT EXISTS "${PP_BROWSER_NO_UI_BINARY}")
  message(FATAL_ERROR "CheckNoUiSymbols: missing binary: ${PP_BROWSER_NO_UI_BINARY}")
endif()

if(NOT PP_BROWSER_NO_UI_SYMTOOL AND PP_BROWSER_NO_UI_NM)
  set(PP_BROWSER_NO_UI_SYMTOOL "${PP_BROWSER_NO_UI_NM}")
  set(PP_BROWSER_NO_UI_SYMTOOL_KIND "nm")
endif()
if(NOT PP_BROWSER_NO_UI_SYMTOOL)
  message(FATAL_ERROR "CheckNoUiSymbols: PP_BROWSER_NO_UI_SYMTOOL not set")
endif()
if(NOT PP_BROWSER_NO_UI_SYMTOOL_KIND)
  set(PP_BROWSER_NO_UI_SYMTOOL_KIND "nm")
endif()

if(PP_BROWSER_NO_UI_SYMTOOL_KIND STREQUAL "dumpbin")
  execute_process(
    COMMAND ${PP_BROWSER_NO_UI_SYMTOOL} /SYMBOLS "${PP_BROWSER_NO_UI_BINARY}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _nm_out
    ERROR_VARIABLE _nm_err
  )
else()
  execute_process(
    COMMAND ${PP_BROWSER_NO_UI_SYMTOOL} -g "${PP_BROWSER_NO_UI_BINARY}"
    RESULT_VARIABLE _rc
    OUTPUT_VARIABLE _nm_out
    ERROR_VARIABLE _nm_err
  )
endif()

if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "CheckNoUiSymbols: ${PP_BROWSER_NO_UI_SYMTOOL_KIND} failed (${_rc}): ${_nm_err}")
endif()

set(_patterns
  "SDL_Init"
  "SDL_CreateWindow"
  "Rml::"
  "_ZN3Rml"
  "_ZN2ui"
  "DebuggerFonts"
)
set(_hits "")
foreach(_pat IN LISTS _patterns)
  string(FIND "${_nm_out}" "${_pat}" _idx)
  if(NOT _idx EQUAL -1)
    list(APPEND _hits "${_pat}")
  endif()
endforeach()

if(_hits)
  string(JOIN ", " _hit_text ${_hits})
  message(FATAL_ERROR
    "${PP_BROWSER_NO_UI_LABEL} linked UI/SDL symbols (${_hit_text}). "
    "pp-node must not depend on RmlUi/SDL. Fix CMake PUBLIC_LIBS edges "
    "(prefer pp_foundation_platform_core).")
endif()
