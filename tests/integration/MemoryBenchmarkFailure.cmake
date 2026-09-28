execute_process(COMMAND "${BENCHMARK}" "${ARGUMENT}" "${VALUE}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error TIMEOUT 10)
string(STRIP "${error}" error)
if(NOT "${result}" STREQUAL "1" OR NOT "${error}" STREQUAL "${EXPECTED}")
    message(FATAL_ERROR "Expected a clean rejected workload, got result=${result}, stderr=${error}")
endif()
if(output MATCHES "\"status\":\"passed\"")
    message(FATAL_ERROR "Rejected workload published a successful summary")
endif()
