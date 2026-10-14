#include <dk/graphics/ShaderCompiler.hpp>
#include <dk/io/File.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#include <cstring>

using namespace dk::graphics;
namespace {
struct Fixture {
    dk::memory::MemorySystem system = std::move(dk::memory::MemorySystem::create().value());
    dk::memory::ResourceHandle heap = system.create_heap({"shader-tests", dk::memory::DomainCategory::render}).value();
    dk::Result<CompiledShader> compile(std::string_view name, std::string_view entry = "computeMain",
                                      ShaderStage stage = ShaderStage::compute)
    {
        return compile_shader({std::filesystem::path{DK_TEST_SHADER_DIR} / name, entry, stage}, heap);
    }
};
void require_success(const dk::Result<CompiledShader>& shader)
{
    if (!shader) { INFO(shader.error().message); FAIL("Shader compilation failed"); }
}
// Read SPIR-V OpEntryPoint independently of Slang reflection.
void check_entry(const CompiledShader& shader, std::uint32_t execution_model, std::string_view name)
{
    REQUIRE(shader.spirv.size() > 5);
    REQUIRE(shader.spirv[0] == 0x07230203U);
    REQUIRE(shader.spirv[1] == 0x00010500U);
    bool found = false;
    for (std::size_t i = 5; i < shader.spirv.size();) {
        const auto count = shader.spirv[i] >> 16;
        REQUIRE(count > 0);
        REQUIRE(i + count <= shader.spirv.size());
        if ((shader.spirv[i] & 0xFFFFU) == 15) {
            REQUIRE(count >= 4);
            CHECK(shader.spirv[i + 1] == execution_model);
            const auto* text = reinterpret_cast<const char*>(&shader.spirv[i + 3]);
            const auto capacity = (count - 3) * sizeof(std::uint32_t);
            const auto* end = static_cast<const char*>(std::memchr(text, '\0', capacity));
            REQUIRE(end != nullptr);
            CHECK(std::string_view(text, static_cast<std::size_t>(end - text)) == name);
            found = true;
        }
        i += count;
    }
    CHECK(found);
}
}

TEST_CASE("graphics entry points emit named SPIR-V", "[shaders]")
{
    Fixture fixture;
    for (const auto stage : {ShaderStage::vertex, ShaderStage::fragment}) {
        const auto name = stage == ShaderStage::vertex ? "vertexMain" : "fragmentMain";
        auto shader = compile_shader({std::filesystem::path{DK_COMMON_SHADER_DIR} / "triangle.slang", name, stage}, fixture.heap);
        require_success(shader);
        check_entry(*shader, stage == ShaderStage::vertex ? 0U : 4U, name);
        CHECK(shader->bindings.empty());
        CHECK(shader->push_constants.empty());
        CHECK(shader->thread_group_size == std::array<std::uint32_t, 3>{});
    }
}

TEST_CASE("ray tracing stages reflect AS descriptors payloads and static selectors", "[shaders]")
{
    Fixture fixture;
    for (const auto stage : {ShaderStage::raygeneration,ShaderStage::miss,ShaderStage::closesthit,ShaderStage::anyhit}) {
        const auto entry=stage==ShaderStage::raygeneration ? "rayMain" : stage==ShaderStage::miss ? "missMain" : stage==ShaderStage::closesthit ? "hitMain" : "anyMain";
        INFO(entry);
        auto shader=fixture.compile("raytracing.slang",entry,stage); require_success(shader);
        auto valid=validate_shader_artifact(*shader); INFO((valid ? "valid" : valid.error().message)); REQUIRE(valid);
        REQUIRE(shader->requires_ray_tracing); REQUIRE(shader->source_fingerprint!=0);
        if (stage==ShaderStage::raygeneration) {
            REQUIRE(shader->bindings[0].type==ShaderDescriptorType::acceleration_structure);
            REQUIRE(shader->ray_interface.traces.size()==2);
            CHECK(shader->ray_interface.traces[1].offset==1);
            CHECK(shader->ray_interface.traces[1].stride==2);
            CHECK(shader->ray_interface.traces[1].miss==1);
        }
        CHECK(nlohmann::json::parse(shader_reflection_json(*shader)).at("requires_ray_tracing")==true);
    }
}

TEST_CASE("ray artifacts reject forged metadata unsupported capabilities and dynamic selectors", "[shaders]")
{
    Fixture fixture;
    auto shader=fixture.compile("raytracing.slang","rayMain",ShaderStage::raygeneration); require_success(shader);
    shader->requires_ray_tracing=false; CHECK_FALSE(validate_shader_artifact(*shader)); shader->requires_ray_tracing=true;
    shader->stage=ShaderStage::miss; CHECK_FALSE(validate_shader_artifact(*shader)); shader->stage=ShaderStage::raygeneration;
    ++shader->ray_interface.traces[0].stride; CHECK_FALSE(validate_shader_artifact(*shader)); --shader->ray_interface.traces[0].stride;
    shader->ray_interface.payloads[0].signature += "forged"; CHECK_FALSE(validate_shader_artifact(*shader));
    shader->ray_interface=reflect_ray_interface(*shader).value(); REQUIRE(validate_shader_artifact(*shader));
    shader->spirv.insert(shader->spirv.begin()+5,{(2u<<16)|17u,11u}); // Int64 is outside enabled features.
    CHECK_FALSE(validate_shader_artifact(*shader));
    const std::array defines{ShaderDefine{"DK_DYNAMIC_SELECTOR","1"}};
    auto dynamic=compile_shader({std::filesystem::path{DK_TEST_SHADER_DIR}/"raytracing.slang","rayMain",ShaderStage::raygeneration,{},defines},fixture.heap);
    REQUIRE_FALSE(dynamic); CHECK(dynamic.error().message.find("static")!=std::string::npos);
}

TEST_CASE("compute reflects storage bindings and push constants", "[shaders]")
{
    Fixture fixture;
    {
        auto shader = compile_shader({std::filesystem::path{DK_COMMON_SHADER_DIR} / "transform.slang", "computeMain"}, fixture.heap);
        require_success(shader);
        check_entry(*shader, 5, "computeMain");
        REQUIRE(shader->bindings.size() == 2);
        for (std::size_t i = 0; i < 2; ++i) {
            CHECK(shader->bindings[i].set == 0);
            CHECK(shader->bindings[i].binding == i);
            CHECK(shader->bindings[i].type == ShaderDescriptorType::storage_buffer);
            CHECK(shader->bindings[i].count == 1);
        }
        REQUIRE(shader->push_constants.size() == 1);
        CHECK(shader->push_constants[0].offset == 0);
        CHECK(shader->push_constants[0].size == 16);
        CHECK(shader->thread_group_size == std::array<std::uint32_t, 3>{64, 1, 1});
        CHECK(shader->spirv.get_allocator().resource() == fixture.heap);
        auto json = nlohmann::json::parse(shader_reflection_json(*shader));
        CHECK(json.at("schema_version") == 3);
        CHECK(json.at("entry") == "computeMain");
        CHECK(json.at("push_constants")[0].at("size") == 16);
        CHECK(fixture.heap.snapshot().live_allocations > 0);
        fixture.system.begin_close();
        check_entry(*shader, 5, "computeMain"); // Owned bytes outlive compiler/session/Memory close request.
    }
    CHECK(fixture.heap.snapshot().live_allocations == 0);
}

TEST_CASE("reflection covers descriptor arrays multiple sets and images", "[shaders]")
{
    Fixture fixture;
    auto shader = fixture.compile("bindings.slang");
    require_success(shader);
    REQUIRE(shader->bindings.size() == 5);
    CHECK(shader->bindings[0].set == 1);
    CHECK(shader->bindings[0].binding == 0);
    CHECK(shader->bindings[0].type == ShaderDescriptorType::sampled_image);
    CHECK(shader->bindings[0].count == 3);
    CHECK(shader->bindings[1].type == ShaderDescriptorType::sampler);
    CHECK(shader->bindings[1].binding == 3);
    CHECK(shader->bindings[2].set == 2);
    CHECK(shader->bindings[2].type == ShaderDescriptorType::storage_buffer);
    CHECK(shader->bindings[3].type == ShaderDescriptorType::storage_image);
    CHECK(shader->bindings[3].binding == 1);
    CHECK(shader->bindings[4].type == ShaderDescriptorType::uniform_buffer);
    CHECK(shader->bindings[4].binding == 4);
    CHECK(shader->bindings[4].block_size == 80);
    CHECK(shader->thread_group_size == std::array<std::uint32_t, 3>{8, 4, 1});
}

TEST_CASE("compilation is repeatable with fresh sessions", "[shaders]")
{
    Fixture fixture;
    auto first = fixture.compile("bindings.slang");
    auto second = fixture.compile("bindings.slang");
    require_success(first); require_success(second);
    CHECK(first->spirv == second->spirv);
    CHECK(shader_reflection_json(*first) == shader_reflection_json(*second));
}

TEST_CASE("compiler preserves useful source entry and stage diagnostics", "[shaders]")
{
    Fixture fixture;
    auto invalid = fixture.compile("invalid.slang");
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().message.find("missing_shader_symbol") != std::string::npos);
    CHECK(invalid.error().message.find("invalid.slang") != std::string::npos);
    REQUIRE(invalid.error().context.size() == 3);
    auto missing = fixture.compile("bindings.slang", "absentMain");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == dk::ErrorCode::not_found);
    auto stage = fixture.compile("bindings.slang", "computeMain", ShaderStage::fragment);
    REQUIRE_FALSE(stage);
    CHECK(stage.error().message.find("stage") != std::string::npos);
    CHECK_FALSE(fixture.compile("no-such-file.slang"));
    CHECK_FALSE(fixture.compile("bindings.slang", "not an identifier"));
    CHECK_FALSE(fixture.compile("bindings.slang", "computeMain", static_cast<ShaderStage>(99)));
    CHECK_FALSE(fixture.compile("unannotated.slang"));
    CHECK_FALSE(compile_shader({std::filesystem::path{DK_TEST_SHADER_DIR} / "bindings.slang", "computeMain"}, {}));
}

TEST_CASE("unsupported shader layouts are rejected explicitly", "[shaders]")
{
    Fixture fixture;
    for (const auto* file : {"parameter-block.slang", "unbounded.slang", "entry-uniform.slang", "duplicate-binding.slang"}) {
        INFO(file);
        auto shader = fixture.compile(file);
        REQUIRE_FALSE(shader);
        INFO(shader.error().message);
        CHECK(shader.error().code == dk::ErrorCode::not_supported);
    }
    CHECK(fixture.heap.snapshot().live_allocations == 0);
}

TEST_CASE("Memory allocation failure cleans partial compiler results", "[shaders]")
{
    Fixture fixture;
    const auto limited = fixture.system.create_heap({"limited-shaders", dk::memory::DomainCategory::render, 128}).value();
    CHECK_THROWS_AS(compile_shader({std::filesystem::path{DK_TEST_SHADER_DIR} / "bindings.slang", "computeMain"}, limited), std::bad_alloc);
    CHECK(limited.snapshot().live_allocations == 0);
    REQUIRE(limited.snapshot().failure_count > 0);
}

TEST_CASE("same process observes include edits and retains successful warnings", "[shaders]")
{
    Fixture fixture;
    const auto directory = std::filesystem::current_path() / "test-artifacts" / "shader-refresh";
    std::filesystem::create_directories(directory);
    const auto source = directory / "refresh.slang";
    const auto include = directory / "value.slang";
    const auto write = [](const std::filesystem::path& path, std::string_view text) {
        REQUIRE(dk::write_file_bytes(path, std::as_bytes(std::span{text.data(), text.size()})));
    };
    write(source, "#include \"value.slang\"\n[[vk::binding(0,0)]] RWStructuredBuffer<uint> values;\n"
        "[shader(\"compute\")] [numthreads(1,1,1)] void computeMain() { values[0] = VALUE; }\n");
    write(include, "#warning shader_success_warning\n#define VALUE 5\n");
    auto first = compile_shader({source, "computeMain"}, fixture.heap);
    require_success(first);
    CHECK(first->diagnostics.find("shader_success_warning") != dk::String::npos);
    const auto original = first->spirv;
    write(include, "#define VALUE 11\n");
    auto second = compile_shader({source, "computeMain"}, fixture.heap);
    require_success(second);
    CHECK(second->spirv != original);
    CHECK(second->source_fingerprint != first->source_fingerprint);
    CHECK(second->diagnostics.empty());
    CHECK(first->spirv == original); // Later compiles cannot invalidate an earlier owned artifact.
}

TEST_CASE("scoped shader batch preserves independent artifacts and cancellation", "[shaders]")
{
    Fixture fixture;
    const std::array requests{
        ShaderCompileRequest{std::filesystem::path{DK_COMMON_SHADER_DIR}/"triangle.slang","vertexMain",ShaderStage::vertex},
        ShaderCompileRequest{std::filesystem::path{DK_COMMON_SHADER_DIR}/"triangle.slang","fragmentMain",ShaderStage::fragment}};
    {
        auto batch=compile_shaders(requests,fixture.heap); REQUIRE(batch); REQUIRE(batch->size()==requests.size());
        for (std::size_t i=0;i<requests.size();++i) {
            auto single=compile_shader(requests[i],fixture.heap); require_success(single);
            CHECK((*batch)[i].spirv==single->spirv);
            CHECK(shader_reflection_json((*batch)[i])==shader_reflection_json(*single));
        }
    }
    std::stop_source stop; stop.request_stop();
    auto cancelled=compile_shaders(requests,fixture.heap,stop.get_token()); REQUIRE_FALSE(cancelled);
    CHECK(cancelled.error().context==std::vector<std::string>{"graphics.shader.compile.cancelled"});
    CHECK(fixture.heap.snapshot().live_allocations==0);
    auto failed_requests=requests; failed_requests[1].entry="missingEntry";
    auto failed=compile_shaders(failed_requests,fixture.heap); REQUIRE_FALSE(failed);
    CHECK(failed.error().code==dk::ErrorCode::not_found);
    CHECK(fixture.heap.snapshot().live_allocations==0);
}

TEST_CASE("reflection distinguishes volume array texture and descriptor array shapes", "[shaders]") {
    Fixture fixture;
    for (const auto file : {"volume-store.slang", "volume-sample.slang"}) {
        auto shader=fixture.compile(file); require_success(shader);
        CHECK(shader->bindings[0].image_shape==ShaderImageShape::image_3d);
        CHECK(nlohmann::json::parse(shader_reflection_json(*shader))["bindings"][0]["image_shape"]=="3d");
        CHECK(validate_shader_artifact(*shader));
        shader->bindings[0].image_shape=ShaderImageShape::none;
        CHECK_FALSE(validate_shader_artifact(*shader));
    }
    auto array=fixture.compile("image-shapes.slang"); require_success(array);
    CHECK(array->bindings[0].count==2); CHECK(array->bindings[0].image_shape==ShaderImageShape::image_2d_array);
    auto depth=fixture.compile("depth-sample.slang"); require_success(depth);
    CHECK(depth->bindings[0].image_shape==ShaderImageShape::image_2d);
    auto rejected=fixture.compile("unsupported-cube.slang"); CHECK_FALSE(rejected);
}

TEST_CASE("sampled image nonuniform capabilities and fixed arrays survive reflection", "[shaders]") {
    Fixture fixture; auto shader=fixture.compile("nonuniform.slang"); require_success(shader);
    REQUIRE(shader->requires_sampled_image_indexing); REQUIRE(validate_shader_artifact(*shader));
    REQUIRE(shader->bindings.size()==3); CHECK(shader->bindings[0].count==64); CHECK(shader->bindings[1].count==64);
    CHECK(shader_reflection_json(*shader).find("requires_sampled_image_indexing")!=std::string::npos);
    shader->requires_sampled_image_indexing=false; CHECK_FALSE(validate_shader_artifact(*shader));
}
