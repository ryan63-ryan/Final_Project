# Runs the kvcache driver on one trace and compares its standard output with
# the expected file. Arguments: PROGRAM, CONFIG, TRACE, EXPECTED.
execute_process(COMMAND "${PROGRAM}" --config "${CONFIG}" --trace "${TRACE}"
  RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "kvcache failed (${status}): ${errors}")
endif()
file(READ "${EXPECTED}" expected)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR "Output mismatch. Actual:\n${actual}\nExpected:\n${expected}")
endif()
