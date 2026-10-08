find_package(Catch2 3 CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(stduuid CONFIG REQUIRED)

add_executable(dk_core_tests ErrorTests.cpp StableIdTests.cpp)
target_link_libraries(dk_core_tests PRIVATE dk::core Catch2::Catch2WithMain Threads::Threads stduuid)
if(DK_BUILD_LOGGING)
    target_sources(dk_core_tests PRIVATE LogTests.cpp)
    target_link_libraries(dk_core_tests PRIVATE dk::logging)
endif()
dk_target_defaults(dk_core_tests)

include(Catch)
if(TARGET dk_scripting_luau)
    add_executable(dk_luau_tests LuauTests.cpp)
    target_link_libraries(dk_luau_tests PRIVATE dk::scripting_luau Catch2::Catch2WithMain)
    target_compile_definitions(dk_luau_tests PRIVATE DK_LUAU_EXAMPLE="${PROJECT_SOURCE_DIR}/examples/scripting/create-scene.luau")
    dk_target_defaults(dk_luau_tests)
    if(TARGET dk_render_services)
        dk_deploy_slang(dk_luau_tests)
    endif()
    catch_discover_tests(dk_luau_tests TEST_PREFIX "dk.luau."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
endif()
if(TARGET dk_geometry)
    add_executable(dk_geometry_tests GeometryTests.cpp)
    target_link_libraries(dk_geometry_tests PRIVATE dk::geometry Catch2::Catch2WithMain)
    dk_target_defaults(dk_geometry_tests)
    catch_discover_tests(dk_geometry_tests TEST_PREFIX "dk.geometry."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS "cpu")
endif()
if(DK_BUILD_RENDER_DISK)
    add_executable(dk_render_disk_tests RenderDiskTests.cpp)
    target_compile_definitions(dk_render_disk_tests PRIVATE DK_DISK_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/render-disk")
    target_link_libraries(dk_render_disk_tests PRIVATE dk::render_disk dk::asset_runtime Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
    dk_target_defaults(dk_render_disk_tests)
    catch_discover_tests(dk_render_disk_tests TEST_PREFIX "dk.render.disk."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS "cpu")
endif()
if(DK_BUILD_RENDER_PIPELINE)
    add_executable(dk_render_pipeline_tests RenderPipelineTests.cpp)
    target_include_directories(dk_render_pipeline_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/render/pipeline/src")
    target_link_libraries(dk_render_pipeline_tests PRIVATE dk::render_pipeline Catch2::Catch2WithMain)
    dk_target_defaults(dk_render_pipeline_tests)
    dk_deploy_slang(dk_render_pipeline_tests)
    catch_discover_tests(dk_render_pipeline_tests TEST_PREFIX "dk.render.pipeline."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS "cpu")
endif()
if(DK_BUILD_RENDER_DATA)
    add_executable(dk_render_data_tests RenderDataTests.cpp)
    target_link_libraries(dk_render_data_tests PRIVATE dk::render_data Catch2::Catch2WithMain)
    dk_target_defaults(dk_render_data_tests)
    catch_discover_tests(dk_render_data_tests TEST_PREFIX "dk.render.data."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS "cpu")
endif()
if(DK_BUILD_RENDER_RESOURCES)
    add_executable(dk_gpu_asset_tests GpuAssetTests.cpp)
    target_link_libraries(dk_gpu_asset_tests PRIVATE dk::render_resources Catch2::Catch2WithMain)
    dk_target_defaults(dk_gpu_asset_tests)
    catch_discover_tests(dk_gpu_asset_tests TEST_PREFIX "dk.render.assets."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS "cpu")
endif()
if(DK_BUILD_GRAPHICS_GRAPH)
    add_executable(dk_graph_tests GraphTests.cpp GraphCompileTests.cpp GraphDiagnosticsTests.cpp)
    target_link_libraries(dk_graph_tests PRIVATE dk::graphics_graph Catch2::Catch2WithMain)
    dk_target_defaults(dk_graph_tests)
    catch_discover_tests(dk_graph_tests TEST_PREFIX "dk.graph."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS "cpu")
endif()
if(DK_BUILD_GRAPHICS_PRESENTATION)
    add_executable(dk_presentation_tests PresentationTests.cpp)
    target_include_directories(dk_presentation_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/graphics/presentation/src")
    target_link_libraries(dk_presentation_tests PRIVATE dk::graphics_presentation Catch2::Catch2WithMain)
    dk_target_defaults(dk_presentation_tests)
    catch_discover_tests(dk_presentation_tests TEST_PREFIX "dk.presentation.unit."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS "cpu")
endif()
if(DK_BUILD_GRAPHICS_OFFSCREEN)
    add_executable(dk_offscreen_tests OffscreenTests.cpp)
    target_include_directories(dk_offscreen_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/graphics/offscreen/src")
    target_link_libraries(dk_offscreen_tests PRIVATE dk::graphics_offscreen Catch2::Catch2WithMain)
    dk_target_defaults(dk_offscreen_tests)
    dk_deploy_slang(dk_offscreen_tests)
    catch_discover_tests(dk_offscreen_tests TEST_PREFIX "dk.offscreen.unit."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS "cpu")
endif()
if(DK_BUILD_GRAPHICS_SHADERS)
    find_package(nlohmann_json CONFIG REQUIRED)
    add_executable(dk_shader_tests ShaderCompilerTests.cpp)
    target_compile_definitions(dk_shader_tests PRIVATE
        DK_TEST_SHADER_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/shaders"
        DK_COMMON_SHADER_DIR="${PROJECT_SOURCE_DIR}/shaders/common")
    target_link_libraries(dk_shader_tests PRIVATE dk::graphics_shaders dk::io Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
    dk_target_defaults(dk_shader_tests)
    dk_deploy_slang(dk_shader_tests)
    catch_discover_tests(dk_shader_tests TEST_PREFIX "dk.shaders."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120 LABELS "cpu")
endif()
if(DK_BUILD_GRAPHICS_DEVICE)
    add_executable(dk_graphics_resource_tests GraphicsResourceTests.cpp)
    target_include_directories(dk_graphics_resource_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/graphics/device/src")
    target_link_libraries(dk_graphics_resource_tests PRIVATE dk::graphics_device Catch2::Catch2WithMain)
    dk_target_defaults(dk_graphics_resource_tests)
    catch_discover_tests(dk_graphics_resource_tests TEST_PREFIX "dk.graphics.unit."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS "cpu")
    add_executable(dk_device_tests DeviceTests.cpp)
    target_include_directories(dk_device_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/graphics/device/src")
    target_link_libraries(dk_device_tests PRIVATE dk::graphics_device Catch2::Catch2WithMain)
    dk_target_defaults(dk_device_tests)
    catch_discover_tests(dk_device_tests TEST_PREFIX "dk.device.unit."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30 LABELS "cpu")
endif()
if(DK_BUILD_JOBS)
    add_executable(dk_jobs_tests JobQueueTests.cpp)
    target_link_libraries(dk_jobs_tests PRIVATE dk::jobs Catch2::Catch2WithMain Threads::Threads)
    dk_target_defaults(dk_jobs_tests)
    catch_discover_tests(dk_jobs_tests TEST_PREFIX "dk.jobs."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 30)
endif()
if(DK_BUILD_ASSET_IMPORTERS)
    find_package(nlohmann_json CONFIG REQUIRED)
    add_executable(dk_import_tests GltfImporterTests.cpp TextureImporterTests.cpp)
    target_compile_definitions(dk_import_tests PRIVATE DK_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/assets")
    target_link_libraries(dk_import_tests PRIVATE dk::asset_importers Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
    dk_target_defaults(dk_import_tests)
    catch_discover_tests(dk_import_tests TEST_PREFIX "dk.import."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
    if(DK_BUILD_ASSET_RUNTIME)
        add_executable(dk_asset_pipeline_tests AssetPipelineTests.cpp)
        target_compile_definitions(dk_asset_pipeline_tests PRIVATE DK_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/assets")
        target_include_directories(dk_asset_pipeline_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/assets/runtime/src")
        target_link_libraries(dk_asset_pipeline_tests PRIVATE dk::asset_runtime Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
        dk_target_defaults(dk_asset_pipeline_tests)
        catch_discover_tests(dk_asset_pipeline_tests TEST_PREFIX "dk.pipeline."
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
        add_executable(dk_asset_cache_tests AssetCacheTests.cpp AssetCacheInvalidationTests.cpp)
        target_compile_definitions(dk_asset_cache_tests PRIVATE DK_TEST_ASSET_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/assets")
        target_include_directories(dk_asset_cache_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/assets/runtime/src")
        target_link_libraries(dk_asset_cache_tests PRIVATE dk::asset_runtime Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
        dk_target_defaults(dk_asset_cache_tests)
        catch_discover_tests(dk_asset_cache_tests TEST_PREFIX "dk.cache."
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
        if(DK_BUILD_JOBS)
            add_executable(dk_async_asset_tests AsyncAssetTests.cpp)
            target_include_directories(dk_async_asset_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/assets/runtime/src")
            target_link_libraries(dk_async_asset_tests PRIVATE dk::asset_runtime Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
            dk_target_defaults(dk_async_asset_tests)
            catch_discover_tests(dk_async_asset_tests TEST_PREFIX "dk.async_assets."
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 45)
        endif()
    endif()
endif()
if(DK_BUILD_ASSET_RUNTIME)
    find_package(nlohmann_json CONFIG REQUIRED)
    add_executable(dk_asset_tests AssetMetadataTests.cpp AssetCatalogTests.cpp AssetDigestTests.cpp AssetPersistenceTests.cpp)
    target_include_directories(dk_asset_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/assets/runtime/src")
    target_link_libraries(dk_asset_tests PRIVATE dk::asset_runtime Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
    if(TARGET dk_asset_services)
        target_sources(dk_asset_tests PRIVATE AssetRegistrationTests.cpp AssetServiceTests.cpp)
        target_link_libraries(dk_asset_tests PRIVATE dk::asset_services)
    endif()
    dk_target_defaults(dk_asset_tests)
    catch_discover_tests(dk_asset_tests TEST_PREFIX "dk.assets."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 120)
endif()
if(DK_BUILD_MEMORY)
    add_executable(dk_memory_tests
        "${PROJECT_SOURCE_DIR}/engine/foundation/memory/tests/MemoryTests.cpp"
        "${PROJECT_SOURCE_DIR}/engine/foundation/memory/tests/OwnershipTests.cpp"
        "${PROJECT_SOURCE_DIR}/engine/foundation/memory/tests/ContextTests.cpp"
        "${PROJECT_SOURCE_DIR}/engine/foundation/memory/tests/ArenaTests.cpp"
        "${PROJECT_SOURCE_DIR}/engine/foundation/memory/tests/PoolTests.cpp")
    target_include_directories(dk_memory_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/foundation/memory/src")
    target_link_libraries(dk_memory_tests PRIVATE dk::memory Catch2::Catch2WithMain Threads::Threads)
    dk_target_defaults(dk_memory_tests)
    catch_discover_tests(dk_memory_tests TEST_PREFIX "dk.memory."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST)
endif()
if(DK_BUILD_FRAMEWORK)
    add_executable(dk_commands_tests CommandTests.cpp)
    target_link_libraries(dk_commands_tests PRIVATE dk::commands Catch2::Catch2WithMain)
    dk_target_defaults(dk_commands_tests)
    catch_discover_tests(dk_commands_tests TEST_PREFIX "dk.commands."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST)
endif()
if(DK_BUILD_SCENE)
    if(DK_BUILD_FRAMEWORK)
        add_executable(dk_protocol_tests ProtocolTests.cpp)
        target_link_libraries(dk_protocol_tests PRIVATE dk::automation_server Catch2::Catch2WithMain)
        dk_target_defaults(dk_protocol_tests)
        if(TARGET dk_render_services)
            target_sources(dk_protocol_tests PRIVATE CaptureTests.cpp)
            target_compile_definitions(dk_protocol_tests PRIVATE DK_CAPTURE_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/render-disk")
            dk_deploy_slang(dk_protocol_tests)
        endif()
        if(TARGET dk_asset_operations)
            add_executable(dk_asset_command_tests AssetCommandTests.cpp)
            target_link_libraries(dk_asset_command_tests PRIVATE dk::automation_server Catch2::Catch2WithMain)
            dk_target_defaults(dk_asset_command_tests)
            catch_discover_tests(dk_asset_command_tests TEST_PREFIX "dk.asset_commands."
                WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 60)
        endif()
        catch_discover_tests(dk_protocol_tests TEST_PREFIX "dk.protocol."
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST)
        add_executable(dk_service_tests ServiceTests.cpp)
        target_link_libraries(dk_service_tests PRIVATE dk::scene_operations Catch2::Catch2WithMain)
        dk_target_defaults(dk_service_tests)
        catch_discover_tests(dk_service_tests TEST_PREFIX "dk.services."
            WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST)
    endif()
    find_package(nlohmann_json CONFIG REQUIRED)
    add_executable(dk_scene_tests SceneDocumentTests.cpp ProjectTests.cpp SceneIOTests.cpp)
    target_link_libraries(dk_scene_tests PRIVATE dk::scene Catch2::Catch2WithMain nlohmann_json::nlohmann_json)
    dk_target_defaults(dk_scene_tests)
    catch_discover_tests(dk_scene_tests TEST_PREFIX "dk.scene."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST)
endif()

if(DK_BUILD_IO)
    add_executable(dk_io_tests IoTests.cpp
        "${PROJECT_SOURCE_DIR}/engine/foundation/io/tests/AtomicFileTests.cpp")
    target_link_libraries(dk_io_tests PRIVATE dk::io Catch2::Catch2WithMain)
    target_compile_definitions(dk_io_tests PRIVATE DK_TEST_CONFIG="$<CONFIG>")
    dk_target_defaults(dk_io_tests)
    catch_discover_tests(dk_io_tests
        TEST_PREFIX "dk.io."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        DISCOVERY_MODE PRE_TEST
    )
endif()

if(DK_BUILD_MATH)
    add_executable(dk_math_tests MathTests.cpp TransformTests.cpp)
    target_link_libraries(dk_math_tests PRIVATE dk::math Catch2::Catch2WithMain)
    dk_target_defaults(dk_math_tests)
    catch_discover_tests(dk_math_tests
        TEST_PREFIX "dk.math."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        DISCOVERY_MODE PRE_TEST
    )
endif()

catch_discover_tests(dk_core_tests
    TEST_PREFIX "dk.core."
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DISCOVERY_MODE PRE_TEST
)

if(DK_BUILD_LOGGING)
    add_executable(dk_log_probe LogProbe.cpp)
    target_link_libraries(dk_log_probe PRIVATE dk::logging)
    dk_target_defaults(dk_log_probe)
    add_test(NAME dk.core.log_stream_separation
        COMMAND "${CMAKE_COMMAND}"
            "-DPROBE=$<TARGET_FILE:dk_log_probe>"
            "-DWORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/test-artifacts/$<CONFIG>/probe"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/LogOutputTest.cmake"
    )
endif()

if(WIN32 AND TARGET dk_automation_server)
    add_executable(dk_ipc_tests IpcTests.cpp)
    target_link_libraries(dk_ipc_tests PRIVATE dk::automation_server dk::automation_client Catch2::Catch2WithMain)
    dk_target_defaults(dk_ipc_tests)
    if(TARGET dk_render_services)
        dk_deploy_slang(dk_ipc_tests)
    endif()
    catch_discover_tests(dk_ipc_tests TEST_PREFIX "dk.ipc." PROPERTIES TIMEOUT 30)
endif()

if(TARGET dk_editor_model)
    add_executable(dk_editor_tests EditorTests.cpp)
    target_link_libraries(dk_editor_tests PRIVATE dk::editor_model Catch2::Catch2WithMain)
    if(WIN32)
        target_sources(dk_editor_tests PRIVATE EditorIpcTests.cpp)
        target_link_libraries(dk_editor_tests PRIVATE dk::automation_client)
    endif()
    target_compile_definitions(dk_editor_tests PRIVATE DK_EDITOR_FIXTURE_DIR="${PROJECT_SOURCE_DIR}/tests/fixtures/render-disk")
    dk_target_defaults(dk_editor_tests)
    if(TARGET dk_render_services)
        dk_deploy_slang(dk_editor_tests)
    endif()
    catch_discover_tests(dk_editor_tests TEST_PREFIX "dk.editor."
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}" DISCOVERY_MODE PRE_TEST PROPERTIES TIMEOUT 60)
endif()
