# Invoked as (pp_browser_add_symbol_check, cmake/PpBrowserNoUiDeps.cmake):
#   cmake -DPP_SYMBOLS_BINARY=... -DPP_SYMBOLS_NM=...
#         -DPP_SYMBOLS_LABEL=... -DPP_SYMBOLS_FORBID=a,b -DPP_SYMBOLS_REQUIRE=c,d -DPP_SYMBOLS_WHY=... -P CheckSymbols.cmake
# Patterns are plain substrings of the symbol listing (',' between them).
if(NOT PP_SYMBOLS_BINARY OR NOT EXISTS "${PP_SYMBOLS_BINARY}")
  message(FATAL_ERROR "CheckSymbols: missing binary: ${PP_SYMBOLS_BINARY}")
endif()

# All symbols, not only -g: a hidden-visibility function is local in the linked binary.
execute_process(COMMAND ${PP_SYMBOLS_NM} "${PP_SYMBOLS_BINARY}"
  RESULT_VARIABLE _rc OUTPUT_VARIABLE _out ERROR_VARIABLE _err)
if(NOT _rc EQUAL 0)
  message(FATAL_ERROR "CheckSymbols: nm failed (${_rc}): ${_err}")
endif()

string(REPLACE "," ";" _forbid "${PP_SYMBOLS_FORBID}")
string(REPLACE "," ";" _require "${PP_SYMBOLS_REQUIRE}")
set(_found "")
foreach(_pat IN LISTS _forbid)
  string(FIND "${_out}" "${_pat}" _idx)
  if(NOT _idx EQUAL -1)
    list(APPEND _found "${_pat}")
  endif()
endforeach()
set(_missing "")
foreach(_pat IN LISTS _require)
  string(FIND "${_out}" "${_pat}" _idx)
  if(_idx EQUAL -1)
    list(APPEND _missing "${_pat}")
  endif()
endforeach()

if(_found)
  string(JOIN ", " _text ${_found})
  message(FATAL_ERROR "${PP_SYMBOLS_LABEL} has symbols it must not (${_text}). ${PP_SYMBOLS_WHY}")
endif()
if(_missing)
  string(JOIN ", " _text ${_missing})
  message(FATAL_ERROR "${PP_SYMBOLS_LABEL} lacks symbols it must have (${_text}). ${PP_SYMBOLS_WHY}")
endif()
