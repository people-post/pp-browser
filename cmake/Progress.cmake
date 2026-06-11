include_guard(GLOBAL)

function(pp_configure_status msg)
  string(TIMESTAMP _pp_ts UTC "%H:%M:%S")
  message(STATUS "[pp-browser ${_pp_ts}] ${msg}")
endfunction()
