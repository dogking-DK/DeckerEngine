#include "../RenderAssetSupport.hpp"
#include "../PathVoxelSupport.hpp"
#include <dk/render/RestirReference.hpp>
#include <dk/render/VolumePathReference.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <random>
using namespace dk;
using namespace dk::render;
using path_voxel_test::Fixture;

TEST_CASE("Sparse blocks preserve constant tiles without expansion", "[voxels][path]") {
    RenderMemory memory; Fixture f{memory.heap};
    for(const unsigned dimension:{8u,128u,4096u}) {
        const std::array blocks{assets::VoxelBlock{{-int(dimension),0,0},dimension,1,false},assets::VoxelBlock{{0,0,0},1,2}};
        f.grid(blocks); const auto partition=f.voxel.voxel->grid->surface_blocks(); REQUIRE(partition);
        REQUIRE(partition->size()==2);
        CHECK_FALSE(f.voxel.voxel->grid->surface_blocks(1));
        const auto scene=f.scene(); const auto hit=scene.nearest({{-double(dimension)/2,.3,.7},{1,0,0}});
        REQUIRE(hit); REQUIRE(*hit); CHECK((*hit)->distance==Catch::Approx(double(dimension)/2+1));
        CHECK((*hit)->cell==geometry::voxels::Coord{0,0,0}); CHECK((*hit)->value==2);
        CHECK((*hit)->kind==PathGeometryKind::voxel); CHECK((*hit)->face==1);
    }
}
TEST_CASE("Native CPU paths match independent exposed triangles", "[voxels][path]") {
    RenderMemory memory; Fixture f{memory.heap}; std::vector<assets::VoxelBlock> blocks;
    std::mt19937 random(2309);
    for(int z=-2;z<2;++z) for(int y=-2;y<2;++y) for(int x=-2;x<2;++x)
        if(random()%3) blocks.push_back({{x,y,z},1,int(random()%2+1)});
    f.grid(blocks);
    for(const double scale:{.00001,1.0,1000.0}) {
        Trsd trs; trs.scale={-scale,2*scale,.5*scale}; trs.translation={2*scale,-scale,3*scale};
        trs.rotation=Eigen::AngleAxisd(.4,Vec3d{1,2,3}.normalized()); f.transform(trs);
        const auto scene=f.scene(),oracle=f.oracle();
        for(int i=0;i<80;++i) {
            Vec3d origin{double(int(random()%900)-450)/100,double(int(random()%900)-450)/100,double(int(random()%900)-450)/100};
            origin+=Vec3d{.00317,.00629,.00913}; // Avoid ambiguous starts exactly on an exposed plane.
            Vec3d dir{double(int(random()%200)-100),double(int(random()%200)-100),double(int(random()%200)-100)};
            PathTraceRay ray{f.world.transform_point(origin).value(),f.world.transform_direction(dir.normalized()).value().normalized()};
            auto a=scene.nearest(ray).value(),b=oracle.nearest(ray).value(); REQUIRE(bool(a)==bool(b));
            if(a) {CHECK(a->kind==PathGeometryKind::voxel); CHECK(a->distance==Catch::Approx(b->distance).margin(scale*2e-5)); CHECK(a->normal.dot(b->normal)>.9999);}
        }
    }
}
TEST_CASE("Voxel content palette and limits are immutable scene identity", "[voxels][path]") {
    RenderMemory memory; Fixture f{memory.heap}; const std::array blocks{assets::VoxelBlock{{0,0,0},1,1}}; f.grid(blocks);
    auto original=f.scene(); const PathTraceRay ray{{.3,.7,2},{0,0,-1}};
    PathTraceSettings s; s.max_bounces=2; s.rr_start=0; s.environment_rgb={1,1,1}; s.estimator=PathTraceEstimator::bsdf_only;
    auto color=estimate_path_reference(original,ray,s,{0,64}); REQUIRE(color);
    CHECK((color->mean-Vec3d{.6f,.4f,.2f}).norm()<1e-6);
    f.voxel.voxel->palette[0].base_color={.1f,.2f,.3f}; auto changed=f.scene();
    CHECK(original.geometry_fingerprint()!=changed.geometry_fingerprint());
    CHECK((estimate_path_reference(original,ray,s,{0,64})->mean-color->mean).norm()<1e-10);
    const std::array moved{assets::VoxelBlock{{2,0,0},1,1}}; f.grid(moved); CHECK(f.scene().geometry_fingerprint()!=changed.geometry_fingerprint());
    CHECK_FALSE(f.scene().nearest(ray).value()); CHECK(original.nearest(ray).value());
    const std::array<const CpuAsset*,2> assets{&f.voxel,&f.mesh}; PathTraceSceneLimits limits; limits.max_voxel_blocks=0;
    CHECK_FALSE(PathTraceScene::create(memory.heap,f.source(),assets,limits));
    CHECK(estimate_restir_reference(original,ray));
    CHECK(original.content_version()!=changed.content_version());
    CHECK(original.rebind(memory.heap,f.source())->content_version()==original.content_version());
    auto medium=PathMediumSnapshot::create(f.document->environment()).value();
    CHECK_FALSE(estimate_volume_path_reference(original,medium,ray));
    f.voxel.voxel->palette[0].metallic=.7f; CHECK_FALSE(PathTraceScene::create(memory.heap,f.source(),assets));
    CHECK(f.scene(PathTraceMaterialModel::metallic_roughness));
    Fixture work{memory.heap}; std::vector<dk::assets::VoxelBlock> strip;
    for(int x=0;x<64;++x) strip.push_back({{x,0,0},1,1+x%2});
    work.grid(strip); auto work_scene=work.scene(); auto bounded=s; bounded.max_bounces=1; bounded.max_triangle_tests=16;
    const PathTraceRay inside{{.3,.4,.7},{1,0,0}};
    CHECK_FALSE(estimate_path_reference(work_scene,inside,bounded,{0,1}));
    bounded.estimator=PathTraceEstimator::mis; CHECK_FALSE(estimate_restir_reference(work_scene,inside,bounded,{}, {0,1}));
    bounded.max_triangle_tests=1024; CHECK(estimate_path_reference(work_scene,inside,bounded,{0,1}));
}
TEST_CASE("Voxel scene allocation failure unwinds all candidate owners", "[voxels][path]") {
    RenderMemory memory; Fixture f{memory.heap}; const std::array blocks{assets::VoxelBlock{{0,0,0},1,1}}; f.grid(blocks);
    const auto original=f.scene(); const auto source=f.source(); const std::array<const CpuAsset*,2> assets{&f.voxel,&f.mesh};
    const auto small=memory.system.create_heap({"voxel-path-budget",memory::DomainCategory::render,32768}).value();
    std::vector<void*> held; while(auto p=small.try_allocate(256)) held.push_back(*p);
    bool failure=false,success=false;
    for(;;) {
        const auto before=small.snapshot();
        {auto candidate=PathTraceScene::create(small,source,assets); failure|=!candidate; success|=bool(candidate);}
        CHECK(small.snapshot().live_allocations==before.live_allocations);
        CHECK(small.snapshot().backing_requested_bytes==before.backing_requested_bytes);
        CHECK(original.nearest({{.3,.7,2},{0,0,-1}}).value());
        if(held.empty()) break;
        small.deallocate(held.back(),256); held.pop_back();
    }
    CHECK(failure); CHECK(success);
}

TEST_CASE("Voxel initial RIS uses native material visibility and frozen content", "[voxels][path][restir]") {
    RenderMemory memory; Fixture f{memory.heap};
    const std::array blocks{assets::VoxelBlock{{-1,0,0},1,1},assets::VoxelBlock{{0,0,0},1,2}}; f.grid(blocks);
    f.quad(-2,2,2,{0,0,0},{2,3,4});
    for(const auto model:{PathTraceMaterialModel::lambert,PathTraceMaterialModel::metallic_roughness}) {
        if(model==PathTraceMaterialModel::metallic_roughness) {f.voxel.voxel->palette[0].metallic=.6f; f.voxel.voxel->palette[0].roughness=.35f;}
        const auto native=f.scene(model),triangles=f.oracle(model),copy=f.scene(model);
        CHECK(native.geometry_fingerprint()==copy.geometry_fingerprint()); CHECK(native.content_version()!=copy.content_version());
        PathTraceSettings s; s.max_bounces=3; s.rr_start=0; s.environment_rgb={.2,.3,.4};
        for(const double x:{-.3,.3}) {
            const PathTraceRay ray{{x,.6,1.5},{0,0,-1}};
            const auto a=estimate_restir_reference(native,ray,s,{}, {0,256});
            const auto b=estimate_restir_reference(triangles,ray,s,{}, {0,256});
            REQUIRE(a); REQUIRE(b); CHECK((a->mean-b->mean).norm()<2e-5);
            CHECK(a->candidate_evaluations==256*8); CHECK(native.nearest(ray).value()->kind==PathGeometryKind::voxel);
        }
    }
}
