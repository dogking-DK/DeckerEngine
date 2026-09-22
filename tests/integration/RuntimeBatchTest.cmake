set(root "${WORK_DIR}/工程 空间")
file(MAKE_DIRECTORY "${root}")
function(invoke file automatic expected output_name)
    set(extra)
    if(automatic)
        list(APPEND extra --auto-guard)
    endif()
    execute_process(COMMAND "${RUNNER}" --project-root "${root}" --batch "${file}" ${extra}
        RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
    if(NOT code EQUAL expected OR NOT errors STREQUAL "")
        message(FATAL_ERROR "batch exit=${code}, expected=${expected}; stderr=${errors}; stdout=${output}")
    endif()
    string(STRIP "${output}" output)
    string(REPLACE ";" "\\;" output "${output}")
    string(REPLACE "\n" ";" lines "${output}")
    set(${output_name} "${lines}" PARENT_SCOPE)
endfunction()
if(CASE STREQUAL "roundtrip")
    file(WRITE "${WORK_DIR}/create.jsonl" [=[
{"jsonrpc":"2.0","id":1,"method":"scene.new","params":{"name":"批处理工程"}}
{"jsonrpc":"2.0","id":2,"method":"entity.create","params":{"id":"11111111-1111-4111-8111-111111111111"}}
{"jsonrpc":"2.0","id":3,"method":"entity.create","params":{"id":"22222222-2222-4222-8222-222222222222"}}
{"jsonrpc":"2.0","id":4,"method":"entity.set_parent","params":{"id":"22222222-2222-4222-8222-222222222222","parent":"11111111-1111-4111-8111-111111111111"}}
{"jsonrpc":"2.0","id":5,"method":"entity.set_name","params":{"id":"22222222-2222-4222-8222-222222222222","name":"子节点"}}
{"jsonrpc":"2.0","id":6,"method":"entity.set_transform","params":{"id":"11111111-1111-4111-8111-111111111111","transform":{"translation":[3,2,1],"rotation":[0,0,0,1],"scale":[2,1,1]}}}
{"jsonrpc":"2.0","id":7,"method":"scene.save"}
{"jsonrpc":"2.0","id":8,"method":"project.save","params":{"manifest":"project.json"}}
{"jsonrpc":"2.0","id":9,"method":"scene.query"}
]=])
    invoke("${WORK_DIR}/create.jsonl" TRUE 0 created)
    list(LENGTH created count)
    if(NOT count EQUAL 9)
        message(FATAL_ERROR "Expected 9 JSON response lines: ${created}")
    endif()
    foreach(line IN LISTS created)
        string(JSON result_type TYPE "${line}" result)
    endforeach()
    list(GET created -1 first)
    string(JSON entities GET "${first}" result value entities)
    string(JSON state GET "${first}" result value state)
    string(JSON translation GET "${entities}" 1 world_matrix 3)
    if(NOT translation EQUAL 3)
        message(FATAL_ERROR "Parent transform did not propagate")
    endif()
    file(WRITE "${WORK_DIR}/load.jsonl" [=[
{"jsonrpc":"2.0","id":10,"method":"scene.load","params":{"manifest":"project.json"}}
{"jsonrpc":"2.0","id":11,"method":"scene.query"}
]=])
    invoke("${WORK_DIR}/load.jsonl" FALSE 0 loaded)
    list(GET loaded -1 second)
    string(JSON loaded_entities GET "${second}" result value entities)
    string(JSON loaded_state GET "${second}" result value state)
    if(NOT entities STREQUAL loaded_entities)
        message(FATAL_ERROR "Persistent entity state changed across processes")
    endif()
    foreach(field IN ITEMS scene_id revision entity_count dirty)
        string(JSON a GET "${state}" ${field})
        string(JSON b GET "${loaded_state}" ${field})
        if(NOT a STREQUAL b)
            message(FATAL_ERROR "Persistent state mismatch: ${field}")
        endif()
    endforeach()
    string(JSON a GET "${state}" document_id)
    string(JSON b GET "${loaded_state}" document_id)
    if(a STREQUAL b)
        message(FATAL_ERROR "Session document ID was persisted")
    endif()
elseif(CASE STREQUAL "errors")
    file(WRITE "${WORK_DIR}/errors.jsonl" [=[
{
{"jsonrpc":"2.0","id":1,"method":"missing"}
{"jsonrpc":"2.0","id":2,"method":"scene.query"}
{"jsonrpc":"2.0","id":3,"method":"scene.new"}
{"jsonrpc":"2.0","id":4,"method":"entity.create"}
{"jsonrpc":"2.0","id":true,"method":"commands.list"}
{"jsonrpc":"2.0","method":"missing"}
[{"jsonrpc":"2.0","id":6,"method":"commands.list"},false,{"jsonrpc":"2.0","method":"commands.list"}]
]=])
    invoke("${WORK_DIR}/errors.jsonl" FALSE 1 lines)
    list(LENGTH lines count)
    if(NOT count EQUAL 7)
        message(FATAL_ERROR "Notification produced a response: ${lines}")
    endif()
    set(expected -32700 -32601 -32002 success -32602 -32600)
    foreach(i RANGE 0 5)
        list(GET lines ${i} line)
        list(GET expected ${i} wanted)
        if(wanted STREQUAL "success")
            string(JSON result_type TYPE "${line}" result)
        else()
            string(JSON actual GET "${line}" error code)
            if(NOT actual EQUAL wanted)
                message(FATAL_ERROR "Wrong error mapping ${actual} != ${wanted}")
            endif()
        endif()
    endforeach()
    list(GET lines 6 batch)
    string(JSON count LENGTH "${batch}")
    if(NOT count EQUAL 2)
        message(FATAL_ERROR "Mixed batch response count")
    endif()
    execute_process(COMMAND "${RUNNER}" --project-root "${root}" --batch "${root}/absent.jsonl"
        RESULT_VARIABLE code OUTPUT_VARIABLE output ERROR_VARIABLE errors)
    if(NOT code EQUAL 2 OR NOT output STREQUAL "" OR errors STREQUAL "")
        message(FATAL_ERROR "Usage/file error stream separation")
    endif()
else()
    message(FATAL_ERROR "Unknown runtime test case")
endif()
