#include <dk/geometry/Query.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <array>
#include <random>
using namespace dk;
using namespace dk::geometry;

TEST_CASE("ray bounds handle parallel inside grazing and invalid input") {
    const Bounds b{{-1,-1,-1},{1,1,1}};
    CHECK(intersect({{0,0,-2},{0,0,1}},b).value().value()==1);
    CHECK(intersect({{1,1,0},{0,0,1}},b).value().value()==0);
    CHECK_FALSE(intersect({{2,0,-2},{0,0,1}},b).value());
    CHECK_FALSE(intersect({{0,0,-2},{0,0,1},0,0.5},b).value());
    CHECK_FALSE(intersect({{0,0,0},{0,0,0}},b));
    CHECK_FALSE(intersect({{0,0,0},{0,0,1},2,1},b));
    CHECK_FALSE(intersect({{0,0,0},{0,0,1}},Bounds{}).value());
    Ray nan; nan.origin.x()=std::numeric_limits<double>::quiet_NaN(); CHECK_FALSE(validate(nan));
}
TEST_CASE("triangle query is double sided bounded and ignores degeneracy") {
    const Triangle t{{-1,-1,0},{1,-1,0},{0,1,0}};
    auto front=intersect({{0,0,-2},{0,0,1}},t).value(); REQUIRE(front);
    CHECK(front->distance==2); CHECK(front->barycentric.sum()==Catch::Approx(1));
    CHECK(intersect({{0,0,2},{0,0,-1}},t).value()->distance==2);
    CHECK_FALSE(intersect({{0,0,-2},{0,0,1},0,1},t).value());
    CHECK_FALSE(intersect({{0,0,-2},{0,0,1}},Triangle{{0,0,0},{0,0,0},{0,0,0}}).value());
    CHECK_FALSE(intersect({{2,0,-2},{0,0,1}},t).value());
}
TEST_CASE("mesh BVH agrees with brute force and retains original triangle indices") {
    std::vector<Triangle> triangles;
    for (int i=0;i<80;++i) { const double x=(i%8)-4.0,z=0.2*(i/8); triangles.push_back({{x-0.4,-0.4,z},{x+0.4,-0.4,z},{x,0.4,z}}); }
    auto mesh=MeshQuery::build(triangles).value();
    std::mt19937 random{123}; std::uniform_real_distribution<double> xy{-5,5};
    for (int sample=0;sample<150;++sample) {
        Ray ray{{xy(random),xy(random)*0.15,-2},{0,0,1}};
        auto hit=mesh.nearest(ray).value(); std::optional<Hit> expected;
        for (std::size_t i=0;i<triangles.size();++i) {
            auto candidate=intersect(ray,triangles[i]).value();
            if (candidate && (!expected || candidate->distance<expected->distance)) { candidate->triangle=i; expected=candidate; }
        }
        REQUIRE(bool(hit)==bool(expected));
        if (hit) { CHECK(hit->triangle==expected->triangle); CHECK(hit->distance==expected->distance); }
    }
    const std::array duplicates{triangles[0],triangles[0]};
    CHECK(MeshQuery::build(duplicates)->nearest({{-4,0,-2},{0,0,1}}).value()->triangle==0);
    CHECK_FALSE(MeshQuery::build({})->nearest(Ray{}).value());
    triangles[0].a.x()=std::numeric_limits<double>::infinity(); CHECK_FALSE(MeshQuery::build(triangles));
    REQUIRE(mesh.nearest({{-4,0,-2},{0,0,1}}).value()); // failed candidate did not affect prior BVH
}
TEST_CASE("mesh instances preserve world ray distance under negative nonuniform scale") {
    const std::array triangles{Triangle{{-1,-1,0},{1,-1,0},{0,1,0}}};
    auto mesh=MeshQuery::build(triangles).value();
    Trsd transform; transform.translation={0,0,3}; transform.scale={-2,3,0.25};
    auto hit=mesh.nearest({{0,0,-2},{0,0,1}},Transformd::from_trs(transform).value()).value();
    REQUIRE(hit); CHECK(hit->distance==Catch::Approx(5));
    transform.scale.z()=10;
    CHECK(mesh.nearest({{0,0,-2},{0,0,1}},Transformd::from_trs(transform).value()).value()->distance==Catch::Approx(5));
    transform.scale.z()=0; CHECK_FALSE(mesh.nearest(Ray{},Transformd::from_trs(transform).value()));
}
