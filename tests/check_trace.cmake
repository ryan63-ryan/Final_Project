# Runs the kvcache driver on one trace and compares its standard output with
# the expected file. Arguments: PROGRAM, CONFIG, TRACE, EXPECTED.
#
# The trace is run twice: with the config as given (reclaimable=count unless it
# says otherwise) and with reclaimable=scan appended. Both outputs must equal
# the expected file byte for byte: the two modes may differ only in speed.
file(READ "${EXPECTED}" expected)
file(READ "${CONFIG}" config_text)
get_filename_component(config_name "${CONFIG}" NAME)
get_filename_component(trace_name "${TRACE}" NAME_WE)
set(scan_config "${CMAKE_CURRENT_BINARY_DIR}/${trace_name}_scan_${config_name}")
file(WRITE "${scan_config}" "${config_text}\nreclaimable=scan\n")
foreach(run "${CONFIG}" "${scan_config}")
  execute_process(COMMAND "${PROGRAM}" --config "${run}" --trace "${TRACE}"
    RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE errors)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "kvcache failed (${status}) with ${run}: ${errors}")
  endif()
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR
      "Output mismatch with ${run}. Actual:\n${actual}\nExpected:\n${expected}")
  endif()
endforeach()
file(REMOVE "${scan_config}")
