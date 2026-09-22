if(TARGET dk_run)
    add_test(NAME dk.bootstrap.version COMMAND dk_run --version)
    string(REPLACE "." "\\." _dk_version_regex "${PROJECT_VERSION}")
    set_tests_properties(dk.bootstrap.version PROPERTIES
        PASS_REGULAR_EXPRESSION "DeckerEngine ${_dk_version_regex}")
endif()

if(TARGET dk_foundation_demo AND WIN32)
    foreach(case IN ITEMS roundtrip overwrite invalid_id nil_id bad_version truncated
            nonfinite projective singular trailing oversized missing_file outside_root missing_parent usage)
        add_test(NAME dk.foundation.${case}
            COMMAND "${CMAKE_COMMAND}"
                "-DDEMO=$<TARGET_FILE:dk_foundation_demo>"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/$<CONFIG>/foundation/${case}"
                "-DCASE=${case}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/FoundationDemoTest.cmake")
    endforeach()
endif()

if(TARGET dk_scene_demo AND WIN32)
    add_test(NAME dk.scene_demo.roundtrip
        COMMAND "${CMAKE_COMMAND}"
            "-DDEMO=$<TARGET_FILE:dk_scene_demo>"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/$<CONFIG>/scene"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/SceneDemoTest.cmake")
endif()
