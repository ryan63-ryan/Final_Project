execute_process(COMMAND "${PROGRAM}" "${INPUT}"
  RESULT_VARIABLE status OUTPUT_VARIABLE actual ERROR_VARIABLE errors)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Demo failed: ${errors}")
endif()
file(READ "${EXPECTED}" expected)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR "Output mismatch. Actual:\n${actual}\nExpected:\n${expected}")
endif()
