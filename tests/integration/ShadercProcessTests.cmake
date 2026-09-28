set(work "${WORK_DIR}/shaderc-unicode-着色器")
file(MAKE_DIRECTORY "${work}/includes" "${work}/out")
file(WRITE "${work}/includes/constants.slang" "static const uint addedValue = 7;\n")
file(WRITE "${work}/includes/math_module.slang" "public uint transformValue(uint value) { return value * 3; }\n")
file(WRITE "${work}/入口.slang" [=[
#include "constants.slang"
import math_module;
[[vk::binding(0, 0)]] RWStructuredBuffer<uint> outputValues;
[shader("compute")]
[numthreads(THREADS, 1, 1)]
void computeMain(uint3 index : SV_DispatchThreadID)
{
    outputValues[index.x] = transformValue(index.x) + addedValue;
}
]=])

function(compile expected source output)
    execute_process(COMMAND "${SHADERC}" compile --source "${source}" --entry computeMain
        --stage compute --output "${output}" ${ARGN}
        WORKING_DIRECTORY "${work}" RESULT_VARIABLE code OUTPUT_VARIABLE json ERROR_VARIABLE diagnostics)
    if(NOT "${code}" STREQUAL "${expected}")
        message(FATAL_ERROR "Expected exit ${expected}, got ${code}: ${diagnostics}\n${json}")
    endif()
    if(expected EQUAL 0)
        string(JSON entry GET "${json}" entry)
        string(JSON version GET "${json}" schema_version)
        if(NOT entry STREQUAL "computeMain" OR NOT version EQUAL 1)
            message(FATAL_ERROR "Invalid reflection: ${json}")
        endif()
        file(READ "${output}" magic LIMIT 4 HEX)
        if(NOT magic STREQUAL "03022307")
            message(FATAL_ERROR "Output is not raw SPIR-V")
        endif()
    elseif(NOT json STREQUAL "" OR diagnostics STREQUAL "")
        message(FATAL_ERROR "Failures must have only stderr diagnostics: ${json} / ${diagnostics}")
    endif()
    set(last_json "${json}" PARENT_SCOPE)
    set(last_diagnostics "${diagnostics}" PARENT_SCOPE)
endfunction()

set(source "${work}/入口.slang")
set(output "${work}/out/结果.spv")
set(options --include "${work}/includes" --define THREADS=8)
compile(0 "${source}" "${output}" ${options})
string(JSON threads GET "${last_json}" thread_group_size 0)
if(NOT threads EQUAL 8)
    message(FATAL_ERROR "Macro did not configure compute group")
endif()
set(first_json "${last_json}")
file(SHA256 "${output}" first_hash)
compile(0 "${source}" "${output}" ${options})
file(SHA256 "${output}" second_hash)
if(NOT first_hash STREQUAL second_hash OR NOT first_json STREQUAL last_json)
    message(FATAL_ERROR "Compilation was not repeatable across processes")
endif()

# Both included and imported changes must be observed by the next compiler invocation.
file(WRITE "${work}/includes/constants.slang" "static const uint addedValue = 19;\n")
compile(0 "${source}" "${output}" ${options})
file(SHA256 "${output}" changed_hash)
if(changed_hash STREQUAL first_hash)
    message(FATAL_ERROR "Include modification was ignored")
endif()
file(WRITE "${work}/includes/math_module.slang" "public uint transformValue(uint value) { return value * 5; }\n")
compile(0 "${source}" "${output}" ${options})
file(SHA256 "${output}" imported_hash)
if(imported_hash STREQUAL changed_hash)
    message(FATAL_ERROR "Import modification was ignored")
endif()

# Diagnostics must retain the file and symbol; prior output remains intact.
compile(1 "${FIXTURES}/invalid.slang" "${output}")
if(NOT last_diagnostics MATCHES "missing_shader_symbol" OR NOT last_diagnostics MATCHES "invalid.slang")
    message(FATAL_ERROR "Source diagnostics were lost: ${last_diagnostics}")
endif()
compile(1 "${source}" "${output}" --include "${work}/includes" --define THREADS=8 --define THREADS=4)
compile(1 "${source}" "${output}" --define THREADS=8)
file(SHA256 "${output}" protected_hash)
if(NOT protected_hash STREQUAL imported_hash)
    message(FATAL_ERROR "Failure modified an existing output")
endif()
compile(1 "${source}" "${work}/missing-parent/output.spv" ${options})
compile(1 "${source}" "${work}/out" ${options})

# Publication may not overwrite a source, include, or imported module.
foreach(input "${source}" "${work}/includes/constants.slang" "${work}/includes/math_module.slang")
    file(SHA256 "${input}" before)
    compile(1 "${source}" "${input}" ${options})
    if(NOT last_diagnostics MATCHES "cannot overwrite")
        message(FATAL_ERROR "Expected dependency protection: ${last_diagnostics}")
    endif()
    file(SHA256 "${input}" after)
    if(NOT before STREQUAL after)
        message(FATAL_ERROR "Shader dependency overwritten")
    endif()
endforeach()

execute_process(COMMAND "${SHADERC}" --help RESULT_VARIABLE code OUTPUT_VARIABLE help)
if(NOT code EQUAL 0 OR NOT help MATCHES "Usage: dk-shaderc")
    message(FATAL_ERROR "Help failed")
endif()
compile(2 "${source}" "${output}" --unknown value)
execute_process(COMMAND "${SHADERC}" compile --source "${source}" --entry computeMain
    --stage geometry --output "${output}" RESULT_VARIABLE code OUTPUT_VARIABLE json ERROR_VARIABLE diagnostics)
if(NOT code EQUAL 2 OR NOT json STREQUAL "" OR diagnostics STREQUAL "")
    message(FATAL_ERROR "Invalid CLI stage must return usage error")
endif()
message(STATUS "shaderc Unicode/include/import/macros/repeatability/diagnostics/publication checks passed")
