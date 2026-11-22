# Runs the kvcache driver twice with --csv on the same file and checks that the
# file holds one header and one row per run, all with the same number of columns.
# The first run also samples the statistics after every operation.
# Arguments: PROGRAM, CONFIG, TRACE, CSV.
file(REMOVE "${CSV}")
foreach(flags "--sample-every;1" "--no-check")
  execute_process(COMMAND "${PROGRAM}" --config "${CONFIG}" --trace "${TRACE}"
    --csv "${CSV}" ${flags} RESULT_VARIABLE status OUTPUT_QUIET ERROR_VARIABLE errors)
  if(NOT status EQUAL 0)
    message(FATAL_ERROR "kvcache failed (${status}): ${errors}")
  endif()
endforeach()
file(STRINGS "${CSV}" lines)
list(LENGTH lines count)
if(NOT count EQUAL 3)
  message(FATAL_ERROR "expected a header and 2 rows, found ${count} lines")
endif()
list(GET lines 0 header)
if(NOT header MATCHES "^capacity,.*,admit_median_ns,.*,total_op_ns,total_driver_ns,peak_rss_kib,reclaimable,stat_samples,mean_used_blocks,mean_fragmentation_slots,peak_fragmentation_slots,mean_duplicate_blocks,mean_live_requests$")
  message(FATAL_ERROR "unexpected header: ${header}")
endif()
string(REGEX MATCHALL "," header_commas "${header}")
list(LENGTH header_commas columns)
foreach(index 1 2)
  list(GET lines ${index} row)
  string(REGEX MATCHALL "," row_commas "${row}")
  list(LENGTH row_commas row_columns)
  if(NOT row_columns EQUAL columns)
    message(FATAL_ERROR "row ${index} has a different number of columns: ${row}")
  endif()
endforeach()
# The first run checks invariants after every operation, the second does not.
list(GET lines 1 checked_row)
list(GET lines 2 unchecked_row)
if(NOT checked_row MATCHES "^4,2,64,lru,on,42,eviction.trace,1,7,0,0,0,19,4,15,0,4,4,4,1,7,")
  message(FATAL_ERROR "unexpected first row: ${checked_row}")
endif()
if(NOT checked_row MATCHES ",count,[0-9]" OR NOT unchecked_row MATCHES ",count,[0-9]")
  message(FATAL_ERROR "the reclaimable column should say which mode ran")
endif()
# 12 operations, sampled after each. Allocated blocks 1,2,3,3,3,3,3,3,4,4,4,4
# (mean 37/12); one empty slot only after the last ADMIT (5 tokens in 3 blocks);
# nothing stored twice; live requests 1,2,3,2,1,0,1,0,1,2,1,2 (mean 16/12).
if(NOT checked_row MATCHES ",count,12,3\\.08333,0\\.0833333,1,0,1\\.33333$")
  message(FATAL_ERROR "unexpected sample columns: ${checked_row}")
endif()
# Sampling is off unless asked for.
if(NOT unchecked_row MATCHES ",count,0,0,0,0,0,0$")
  message(FATAL_ERROR "unexpected sample columns without --sample-every: ${unchecked_row}")
endif()
if(NOT unchecked_row MATCHES "^4,2,64,lru,on,42,eviction.trace,0,7,0,0,0,19,4,15,0,4,4,4,1,7,")
  message(FATAL_ERROR "unexpected second row: ${unchecked_row}")
endif()
