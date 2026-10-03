#include "../RenderAssetSupport.hpp"
#include <dk/render/GpuAssets.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <limits>

using namespace dk;
TEST_CASE("GPU asset validates mesh materials textures and sampler without a device") {
    RenderMemory f; auto a = render_asset();
    REQUIRE(render::validate_gpu_asset(a));
    const auto scenario = GENERATE(0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);
    switch (scenario) {
    case 0: a.mesh.id = {}; break;
    case 1: a.mesh.primitives.clear(); break;
    case 2: a.mesh.primitives[0].indices[2] = 3; break;
    case 3: a.mesh.primitives[0].indices.pop_back(); break;
    case 4: a.mesh.primitives[0].normals.resize(1); break;
    case 5: a.mesh.primitives[0].positions[0].x() = std::numeric_limits<float>::infinity(); break;
    case 6: a.mesh.primitives[0].material = AssetId{}; break;
    case 7: a.materials[0].id = a.mesh.id; break;
    case 8: a.materials[1].id = a.materials[0].id; break;
    case 9: a.materials[0].base_color_texture = AssetId{}; break;
    case 10: a.materials[0].base_color.w() = -1; break;
    case 11: a.materials[0].roughness = std::numeric_limits<float>::quiet_NaN(); break;
    case 12: a.textures[0].rgba8.pop_back(); break;
    case 13: a.textures[0].width = 0; break;
    case 14: a.textures[0].width = a.textures[0].height = std::numeric_limits<std::uint32_t>::max(); break;
    case 15: a.textures[0].sampler.mag_filter = TextureFilter::linear_mipmap_linear; break;
    case 16: a.textures[0].sampler.wrap_t = static_cast<TextureWrap>(0); break;
    case 17: a.textures[1].id = a.textures[0].id; break;
    }
    CHECK_FALSE(render::validate_gpu_asset(a));
}
TEST_CASE("GPU cache empty moved and closing states do not retain entries") {
    RenderMemory f;
    render::GpuAssets empty; CHECK(empty.size() == 0); CHECK_FALSE(empty.find({})); CHECK_FALSE(empty.unload({})); empty.clear();
    CHECK_FALSE(render::GpuAssets::create({}));
    auto cache = render::GpuAssets::create(f.heap).value(); auto moved = std::move(cache);
    CHECK(cache.size() == 0); CHECK(moved.size() == 0);
    f.heap.begin_close(); CHECK_FALSE(render::GpuAssets::create(f.heap)); moved.clear();
    render::GpuAsset snapshot; CHECK_FALSE(snapshot); CHECK(snapshot.mesh().primitives.empty());
    CHECK(snapshot.textures().empty()); CHECK(snapshot.materials().empty()); CHECK(snapshot.generation() == 0);
}
