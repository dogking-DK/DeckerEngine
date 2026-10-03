if(DK_BUILD_RENDER_DISK AND DK_BUILD_RENDER_PIPELINE)
    add_executable(dk_render_disk_probe RenderDiskProbe.cpp)
    set_target_properties(dk_render_disk_probe PROPERTIES OUTPUT_NAME dk-render-disk-probe)
    target_compile_definitions(dk_render_disk_probe PRIVATE DK_DISK_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/render-disk"
        DK_RENDER_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders/render")
    target_include_directories(dk_render_disk_probe PRIVATE "${PROJECT_SOURCE_DIR}/engine/graphics/device/src")
    target_link_libraries(dk_render_disk_probe PRIVATE dk::render_disk dk::render_pipeline dk::asset_runtime)
    dk_target_defaults(dk_render_disk_probe)
    dk_deploy_slang(dk_render_disk_probe)
    add_test(NAME dk.render.disk_gpu_validation COMMAND dk_render_disk_probe)
    set_tests_properties(dk.render.disk_gpu_validation PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "gpu"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1;VK_VALIDATION_VALIDATE_SYNC=1")
    if(TARGET dk_render_demo)
        add_test(NAME dk.render.sponza_validation COMMAND dk_render_demo)
        set_tests_properties(dk.render.sponza_validation PROPERTIES TIMEOUT 180 SKIP_RETURN_CODE 77 LABELS "gpu;local-assets"
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1;VK_VALIDATION_VALIDATE_SYNC=1")
    endif()
endif()
if(DK_BUILD_RENDER_PIPELINE)
    add_executable(dk_render_pipeline_probe RenderPipelineProbe.cpp)
    set_target_properties(dk_render_pipeline_probe PROPERTIES OUTPUT_NAME "dk-render-pipeline-probe")
    target_compile_definitions(dk_render_pipeline_probe PRIVATE DK_RENDER_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders/render")
    target_include_directories(dk_render_pipeline_probe PRIVATE "${PROJECT_SOURCE_DIR}/engine/graphics/device/src")
    target_link_libraries(dk_render_pipeline_probe PRIVATE dk::render_pipeline dk::graphics_graph)
    dk_target_defaults(dk_render_pipeline_probe)
    dk_deploy_slang(dk_render_pipeline_probe)
    add_test(NAME dk.render.pipeline_gpu_validation COMMAND dk_render_pipeline_probe)
    set_tests_properties(dk.render.pipeline_gpu_validation PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "gpu"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1;VK_VALIDATION_VALIDATE_SYNC=1")
endif()
if(DK_BUILD_RENDER_RESOURCES)
    add_executable(dk_render_resources_probe RenderResourcesProbe.cpp)
    set_target_properties(dk_render_resources_probe PROPERTIES OUTPUT_NAME "dk-render-resources-probe")
    target_include_directories(dk_render_resources_probe PRIVATE "${PROJECT_SOURCE_DIR}/engine/graphics/device/src")
    target_link_libraries(dk_render_resources_probe PRIVATE dk::render_resources)
    dk_target_defaults(dk_render_resources_probe)
    add_test(NAME dk.render.gpu_resources COMMAND dk_render_resources_probe)
    add_test(NAME dk.render.gpu_validation COMMAND dk_render_resources_probe --validation)
    set_tests_properties(dk.render.gpu_resources dk.render.gpu_validation PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "gpu")
    set_tests_properties(dk.render.gpu_validation PROPERTIES ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1;VK_VALIDATION_VALIDATE_SYNC=1")
endif()
if(DK_BUILD_GRAPHICS_GRAPH)
    add_executable(dk_graph_probe GraphProbe.cpp)
    set_target_properties(dk_graph_probe PROPERTIES OUTPUT_NAME "dk-graph-probe")
    target_include_directories(dk_graph_probe PRIVATE "${PROJECT_SOURCE_DIR}/engine/graphics/device/src")
    target_link_libraries(dk_graph_probe PRIVATE dk::graphics_graph)
    dk_target_defaults(dk_graph_probe)
    add_test(NAME dk.graph.gpu_execution COMMAND dk_graph_probe)
    add_test(NAME dk.graph.gpu_validation COMMAND dk_graph_probe --validation)
    set_tests_properties(dk.graph.gpu_execution dk.graph.gpu_validation PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "gpu")
    set_tests_properties(dk.graph.gpu_validation PROPERTIES ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1;VK_VALIDATION_VALIDATE_SYNC=1")
endif()
if(TARGET dk_shaderc)
    add_test(NAME dk.shaderc.process COMMAND "${CMAKE_COMMAND}"
        "-DSHADERC=$<TARGET_FILE:dk_shaderc>"
        "-DFIXTURES=${PROJECT_SOURCE_DIR}/tests/fixtures/shaders"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/ShadercProcessTests.cmake")
    set_tests_properties(dk.shaderc.process PROPERTIES TIMEOUT 120 LABELS "cpu")
    if(WIN32)
        add_test(NAME dk.shaderc.deployment COMMAND "${CMAKE_COMMAND}"
            "-DSHADERC=$<TARGET_FILE:dk_shaderc>"
            "-DSLANG=$<TARGET_FILE:slang::slang>"
            "-DOPTIMIZER=$<TARGET_FILE:slang::slang-glslang>"
            "-DSOURCE=${PROJECT_SOURCE_DIR}/shaders/common/transform.slang"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/ShadercDeploymentTests.cmake")
        set_tests_properties(dk.shaderc.deployment PROPERTIES TIMEOUT 120 LABELS "cpu")
    endif()
endif()
if(TARGET dk_assetc)
    add_test(NAME dk.assetc.process COMMAND "${CMAKE_COMMAND}"
        "-DASSETC=$<TARGET_FILE:dk_assetc>"
        "-DFIXTURES=${PROJECT_SOURCE_DIR}/tests/fixtures/assets"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/AssetcProcessTests.cmake")
    set_tests_properties(dk.assetc.process PROPERTIES TIMEOUT 120)
    add_test(NAME dk.assetc.cache_process COMMAND "${CMAKE_COMMAND}"
        "-DASSETC=$<TARGET_FILE:dk_assetc>"
        "-DFIXTURES=${PROJECT_SOURCE_DIR}/tests/fixtures/assets"
        "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/AssetCacheProcessTests.cmake")
    set_tests_properties(dk.assetc.cache_process PROPERTIES TIMEOUT 120)
endif()

if(DK_BUILD_PLATFORM)
    find_package(SDL3 CONFIG REQUIRED)
    find_package(SDL3 CONFIG REQUIRED)
    add_executable(dk_platform_probe PlatformProbe.cpp)
    target_include_directories(dk_platform_probe PRIVATE "${PROJECT_SOURCE_DIR}/engine/platform/src")
    target_link_libraries(dk_platform_probe PRIVATE dk::platform SDL3::SDL3)
    dk_target_defaults(dk_platform_probe)
    add_test(NAME dk.platform.windows COMMAND dk_platform_probe)
    set_tests_properties(dk.platform.windows PROPERTIES TIMEOUT 30 LABELS "desktop")
endif()
if(TARGET dk_presentation_demo)
    add_test(NAME dk.presentation.example_smoke COMMAND dk_presentation_demo --validation --frames 8)
    set_tests_properties(dk.presentation.example_smoke PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS "gpu;desktop"
        PASS_REGULAR_EXPRESSION "presented=8.*errors=0 warnings=0"
        ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1;VK_VALIDATION_VALIDATE_SYNC=1")
endif()
if(DK_BUILD_GRAPHICS_PRESENTATION)
    add_executable(dk_present_device_probe PresentDeviceProbe.cpp)
    target_link_libraries(dk_present_device_probe PRIVATE dk::graphics_presentation)
    dk_target_defaults(dk_present_device_probe)
    add_test(NAME dk.presentation.device_validation COMMAND dk_present_device_probe)
    set_tests_properties(dk.presentation.device_validation PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS "gpu;desktop")
    if(DK_BUILD_GRAPHICS_SHADERS)
        add_executable(dk_presentation_probe PresentationProbe.cpp)
        target_compile_definitions(dk_presentation_probe PRIVATE DK_COMMON_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders/common")
        target_include_directories(dk_presentation_probe PRIVATE "${PROJECT_SOURCE_DIR}/engine/graphics/presentation/src"
            "${PROJECT_SOURCE_DIR}/engine/graphics/device/src")
        target_link_libraries(dk_presentation_probe PRIVATE dk::graphics_presentation dk::graphics_shaders)
        dk_target_defaults(dk_presentation_probe)
        dk_deploy_slang(dk_presentation_probe)
        add_test(NAME dk.presentation.frames_validation COMMAND dk_presentation_probe)
        add_test(NAME dk.presentation.recovery_validation COMMAND dk_presentation_probe --recovery)
        set_tests_properties(dk.presentation.frames_validation dk.presentation.recovery_validation PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "gpu;desktop"
            ENVIRONMENT "VK_LAYER_VALIDATE_SYNC=1;VK_VALIDATION_VALIDATE_SYNC=1")
    endif()
endif()
if(DK_BUILD_GRAPHICS_OFFSCREEN)
    add_executable(dk_offscreen_probe OffscreenProbe.cpp GraphPipeline.cpp)
    set_target_properties(dk_offscreen_probe PROPERTIES OUTPUT_NAME "dk-offscreen-probe")
    target_compile_definitions(dk_offscreen_probe PRIVATE DK_COMMON_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders/common")
    target_include_directories(dk_offscreen_probe PRIVATE "${PROJECT_SOURCE_DIR}/engine/graphics/offscreen/src"
        "${PROJECT_SOURCE_DIR}/engine/graphics/device/src")
    target_link_libraries(dk_offscreen_probe PRIVATE dk::graphics_offscreen dk::graphics_graph)
    dk_target_defaults(dk_offscreen_probe)
    dk_deploy_slang(dk_offscreen_probe)
    add_test(NAME dk.offscreen.gpu_execute COMMAND dk_offscreen_probe)
    add_test(NAME dk.offscreen.gpu_validation COMMAND dk_offscreen_probe --validation)
    set_tests_properties(dk.offscreen.gpu_execute dk.offscreen.gpu_validation PROPERTIES
        TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "gpu" WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    set_tests_properties(dk.offscreen.gpu_validation PROPERTIES ENVIRONMENT
        "VK_LAYER_VALIDATE_SYNC=1;VK_VALIDATION_VALIDATE_SYNC=1")
endif()
if(DK_BUILD_GRAPHICS_DEVICE)
    add_executable(dk_graphics_resource_probe GraphicsResourceProbe.cpp)
    set_target_properties(dk_graphics_resource_probe PROPERTIES OUTPUT_NAME "dk-graphics-resource-probe")
    target_include_directories(dk_graphics_resource_probe PRIVATE "${PROJECT_SOURCE_DIR}/engine/graphics/device/src")
    target_link_libraries(dk_graphics_resource_probe PRIVATE dk::graphics_device)
    dk_target_defaults(dk_graphics_resource_probe)
    add_test(NAME dk.graphics.gpu_resources COMMAND dk_graphics_resource_probe)
    add_test(NAME dk.graphics.gpu_resources_validation COMMAND dk_graphics_resource_probe --validation)
    set_tests_properties(dk.graphics.gpu_resources dk.graphics.gpu_resources_validation PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 LABELS "gpu")
    # SDK 1.4.321 uses VK_LAYER_; newer validation layers use VK_VALIDATION_.
    set_tests_properties(dk.graphics.gpu_resources_validation PROPERTIES ENVIRONMENT
        "VK_LAYER_VALIDATE_SYNC=1;VK_VALIDATION_VALIDATE_SYNC=1")
    add_executable(dk_device_probe DeviceProbe.cpp)
    set_target_properties(dk_device_probe PROPERTIES OUTPUT_NAME "dk-device-probe")
    target_link_libraries(dk_device_probe PRIVATE dk::graphics_device)
    dk_target_defaults(dk_device_probe)
    add_test(NAME dk.device.gpu_smoke COMMAND dk_device_probe)
    add_test(NAME dk.device.gpu_validation COMMAND dk_device_probe --validation)
    set_tests_properties(dk.device.gpu_smoke dk.device.gpu_validation PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77 LABELS "gpu")
endif()

add_executable(dk_profiling_probe ProfilingProbe.cpp)
target_link_libraries(dk_profiling_probe PRIVATE dk::profiling)
find_package(Threads REQUIRED)
if(DK_BUILD_MEMORY)
    add_executable(dk_memory_benchmark MemoryBenchmark.cpp)
    target_link_libraries(dk_memory_benchmark PRIVATE dk::memory dk::profiling Threads::Threads)
    target_compile_definitions(dk_memory_benchmark PRIVATE DK_BENCH_CONFIGURATION="$<CONFIG>")
    dk_target_defaults(dk_memory_benchmark)
    add_test(NAME dk.memory.benchmark_smoke COMMAND dk_memory_benchmark)
    set_tests_properties(dk.memory.benchmark_smoke PROPERTIES
        TIMEOUT 60 PASS_REGULAR_EXPRESSION "\"status\":\"passed\",\"case_count\":40")
    add_test(NAME dk.memory.benchmark_invalid_argument COMMAND "${CMAKE_COMMAND}"
        "-DBENCHMARK=$<TARGET_FILE:dk_memory_benchmark>" -DARGUMENT=--rounds -DVALUE=0
        "-DEXPECTED=invalid bounded benchmark parameter" -P "${CMAKE_CURRENT_SOURCE_DIR}/MemoryBenchmarkFailure.cmake")
    add_test(NAME dk.memory.benchmark_partial_failure COMMAND "${CMAKE_COMMAND}"
        "-DBENCHMARK=$<TARGET_FILE:dk_memory_benchmark>" -DARGUMENT=--allocation-budget -DVALUE=64
        "-DEXPECTED=heap allocation" -P "${CMAKE_CURRENT_SOURCE_DIR}/MemoryBenchmarkFailure.cmake")
    set_tests_properties(dk.memory.benchmark_invalid_argument dk.memory.benchmark_partial_failure PROPERTIES TIMEOUT 15)
    add_executable(dk_context_probe ContextProbe.cpp)
    target_link_libraries(dk_context_probe PRIVATE dk::memory dk::profiling Threads::Threads)
    dk_target_defaults(dk_context_probe)
    add_test(NAME dk.memory.context_probe COMMAND dk_context_probe)
    set_tests_properties(dk.memory.context_probe PROPERTIES
        TIMEOUT 15 PASS_REGULAR_EXPRESSION "\"status\":\"passed\".*\"systems\":2,\"tasks\":3")
    add_executable(dk_pool_probe PoolProbe.cpp)
    target_link_libraries(dk_pool_probe PRIVATE dk::memory dk::profiling Threads::Threads)
    dk_target_defaults(dk_pool_probe)
    add_test(NAME dk.memory.pool_probe COMMAND dk_pool_probe)
    set_tests_properties(dk.memory.pool_probe PROPERTIES
        TIMEOUT 15 PASS_REGULAR_EXPRESSION "\"status\":\"passed\".*\"requests\":192")
    add_executable(dk_arena_probe ArenaProbe.cpp)
    target_link_libraries(dk_arena_probe PRIVATE dk::memory dk::profiling Threads::Threads)
    dk_target_defaults(dk_arena_probe)
    add_test(NAME dk.memory.arena_probe COMMAND dk_arena_probe)
    set_tests_properties(dk.memory.arena_probe PROPERTIES
        TIMEOUT 15 PASS_REGULAR_EXPRESSION "arena=(on|off);chunks=3;requests=102")
    add_executable(dk_memory_probe MemoryProbe.cpp)
    target_link_libraries(dk_memory_probe PRIVATE dk::memory dk::profiling Threads::Threads)
    dk_target_defaults(dk_memory_probe)
    add_test(NAME dk.memory.probe COMMAND dk_memory_probe)
    set_tests_properties(dk.memory.probe PROPERTIES
        TIMEOUT 15 PASS_REGULAR_EXPRESSION "memory=(on|off);allocations=36")
endif()
target_link_libraries(dk_profiling_probe PRIVATE Threads::Threads)
dk_target_defaults(dk_profiling_probe)
add_test(NAME dk.profiling.smoke COMMAND dk_profiling_probe)
set_tests_properties(dk.profiling.smoke PROPERTIES
    TIMEOUT 15 PASS_REGULAR_EXPRESSION "profiling=(on|off);checksum=8128")

# Compile in isolation even in the ON build: no Tracy include path or link usage.
add_executable(dk_profiling_disabled_test ProfilingDisabledTest.cpp)
target_include_directories(dk_profiling_disabled_test PRIVATE
    "${PROJECT_SOURCE_DIR}/engine/foundation/profiling/include")
target_compile_definitions(dk_profiling_disabled_test PRIVATE DK_ENABLE_PROFILING=0)
dk_target_defaults(dk_profiling_disabled_test)
add_test(NAME dk.profiling.disabled_no_side_effects COMMAND dk_profiling_disabled_test)

if(TARGET dk_run)
    add_test(NAME dk.bootstrap.version COMMAND dk_run --version)
    string(REPLACE "." "\\." _dk_version_regex "${PROJECT_VERSION}")
    set_tests_properties(dk.bootstrap.version PROPERTIES
        PASS_REGULAR_EXPRESSION "DeckerEngine ${_dk_version_regex}")
endif()
if(TARGET dk_asset_services AND WIN32)
    add_executable(dk_asset_recovery_probe AssetRecoveryProbe.cpp)
    target_include_directories(dk_asset_recovery_probe PRIVATE "${PROJECT_SOURCE_DIR}/engine/assets/runtime/src")
    target_link_libraries(dk_asset_recovery_probe PRIVATE dk::asset_services)
    dk_target_defaults(dk_asset_recovery_probe)
    foreach(kind IN ITEMS registration rename)
        foreach(point IN ITEMS source meta manifest finish)
            add_test(NAME dk.assets.restart_${kind}_${point}
                COMMAND "${CMAKE_COMMAND}" "-DPROBE=$<TARGET_FILE:dk_asset_recovery_probe>"
                    "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/$<CONFIG>/assets"
                    "-DKIND=${kind}" "-DPOINT=${point}" -P "${CMAKE_CURRENT_SOURCE_DIR}/AssetRecoveryTest.cmake")
            set_tests_properties(dk.assets.restart_${kind}_${point} PROPERTIES TIMEOUT 30)
        endforeach()
    endforeach()
endif()
if(TARGET dk_runtime AND TARGET dk_run AND WIN32)
    find_program(DK_POWERSHELL NAMES pwsh powershell REQUIRED)
    if(TARGET dk_asset_operations)
        add_test(NAME dk.runtime.assets_stdio
            COMMAND "${DK_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
                -File "${CMAKE_CURRENT_SOURCE_DIR}/RuntimeAssetsTest.ps1"
                -Runner "$<TARGET_FILE:dk_run>" -Fixtures "${PROJECT_SOURCE_DIR}/tests/fixtures/assets"
                -WorkDir "${PROJECT_BINARY_DIR}/test-artifacts/$<CONFIG>")
        set_tests_properties(dk.runtime.assets_stdio PROPERTIES TIMEOUT 90)
    endif()
    add_test(NAME dk.runtime.stdio_interactive
        COMMAND "${DK_POWERSHELL}" -NoProfile -ExecutionPolicy Bypass
            -File "${CMAKE_CURRENT_SOURCE_DIR}/RuntimeStdioTest.ps1"
            -Runner "$<TARGET_FILE:dk_run>"
            -WorkDir "${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/$<CONFIG>/runtime/stdio")
    set_tests_properties(dk.runtime.stdio_interactive PROPERTIES TIMEOUT 60)
    foreach(case IN ITEMS roundtrip errors)
        add_test(NAME dk.runtime.batch_${case}
            COMMAND "${CMAKE_COMMAND}"
                "-DRUNNER=$<TARGET_FILE:dk_run>"
                "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/$<CONFIG>/runtime/${case}"
                "-DCASE=${case}"
                -P "${CMAKE_CURRENT_SOURCE_DIR}/RuntimeBatchTest.cmake")
    endforeach()
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
