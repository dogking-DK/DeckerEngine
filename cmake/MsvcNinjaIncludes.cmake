# Ninja matches cl /showIncludes against the compiler's raw output. CMake's
# console-code-page conversion can change those bytes in a localized toolset.
set(_dk_probe_dir "${CMAKE_BINARY_DIR}/CMakeFiles/dk-show-includes")
file(WRITE "${_dk_probe_dir}/probe.cpp" "#include \"dk-includes-probe.hpp\"\n")
file(WRITE "${_dk_probe_dir}/dk-includes-probe.hpp" "\n")
execute_process(
    COMMAND "${CMAKE_CXX_COMPILER}" /nologo /utf-8 /showIncludes /Zs probe.cpp
    WORKING_DIRECTORY "${_dk_probe_dir}"
    OUTPUT_VARIABLE _dk_includes ERROR_VARIABLE _dk_errors
    RESULT_VARIABLE _dk_status ENCODING NONE)
if(NOT _dk_status EQUAL 0 OR NOT _dk_includes MATCHES
        "(^|\n)([^\r\n]+[ \t])([A-Za-z]:[/\\\\]|[/\\\\][/\\\\])[^\r\n]*[/\\\\]dk-includes-probe[.]hpp(\r?\n|$)")
    message(FATAL_ERROR "Cannot detect MSVC /showIncludes for Ninja: ${_dk_includes}${_dk_errors}")
endif()
set(CMAKE_CL_SHOWINCLUDES_PREFIX "${CMAKE_MATCH_2}")
