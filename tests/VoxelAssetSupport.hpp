#pragma once
#include <dk/assets/VoxelImporter.hpp>
#include <dk/io/File.hpp>
#include <openvdb/openvdb.h>
#include <openvdb/io/Stream.h>
#include <nlohmann/json.hpp>
#include <fstream>
namespace voxel_fixture {
using Json=nlohmann::json;
template<class T> T take(dk::Result<T> r) { if(!r) throw std::runtime_error(r.error().message); return std::move(*r); }
inline void check(dk::Result<void> r) { if(!r) throw std::runtime_error(r.error().message); }
inline Json description() {
    return {{"format","DeckerVoxelSource"},{"version",1},{"vdb","grid.vdb"},{"grid","solid"},{"semantics","solid-material-index"},
        {"transform","vdb-sample-center"},{"domain",{{"minimum",{-8,0,0}},{"maximum",{7,7,7}}}},
        {"palette",Json::array({{{"base_color",{.125,.25,1.}},{"metallic",0},{"roughness",1}},{{"base_color",{1.,.5,.125}},{"metallic",0},{"roughness",.5}}})}};
}
inline void write_text(const std::filesystem::path& file,const Json& j) {
    const auto s=j.dump(2)+'\n'; check(dk::write_file_bytes_atomic(file,std::as_bytes(std::span{s.data(),s.size()})));
}
inline openvdb::Int32Grid::Ptr grid(int value=1) {
    openvdb::initialize(); auto g=openvdb::Int32Grid::create(0); g->setName("solid");
    g->tree().addTile(1,{-8,0,0},value,false); g->tree().setValue({0,0,0},2); g->tree().setValue({1,1,1},0);
    auto transform=openvdb::math::Transform::createLinearTransform(.125); transform->postTranslate(openvdb::Vec3d(.0625)); g->setTransform(transform); return g;
}
inline void write_vdb(const std::filesystem::path& file,openvdb::GridBase::Ptr selected=grid()) {
    auto density=openvdb::FloatGrid::create(0); density->setName("density"); density->setGridClass(openvdb::GRID_FOG_VOLUME);
    auto alternate=grid(2); alternate->setName("alternate");
    std::ofstream out(file,std::ios::binary); openvdb::io::Stream archive(out); archive.write(openvdb::GridPtrVec{density,selected,alternate});
    out.close(); if(!out) throw std::runtime_error("VDB fixture write failed");
}
inline void write(const std::filesystem::path& root) {
    std::filesystem::create_directories(root/"assets"); write_vdb(root/"assets/grid.vdb"); write_text(root/"assets/solid.dkvoxel",description());
}
inline std::string read(const std::filesystem::path& p) { const auto b=take(dk::read_file_bytes(p)); return {reinterpret_cast<const char*>(b.data()),b.size()}; }
} // namespace voxel_fixture
