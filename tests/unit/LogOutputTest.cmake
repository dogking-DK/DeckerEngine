file(MAKE_DIRECTORY "${WORK_DIR}")
execute_process(
    COMMAND "${PROBE}" "${WORK_DIR}/output.log"
    RESULT_VARIABLE result
    OUTPUT_VARIABLE protocol
    ERROR_VARIABLE diagnostics
)
if(NOT result STREQUAL "0")
    message(FATAL_ERROR "Logger probe failed (${result}): ${diagnostics}")
endif()
string(REPLACE "\r\n" "\n" protocol "${protocol}")
if(NOT protocol STREQUAL "protocol-result\n")
    message(FATAL_ERROR "Unexpected protocol stdout: ${protocol}")
endif()
if(NOT diagnostics MATCHES "\\[probe\\] diagnostic 42")
    message(FATAL_ERROR "Missing diagnostic on stderr: ${diagnostics}")
endif()
file(READ "${WORK_DIR}/output.log" contents)
if(NOT contents MATCHES "\\[probe\\] diagnostic 42")
    message(FATAL_ERROR "Missing diagnostic in log file")
endif()
