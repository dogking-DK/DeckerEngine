#pragma once
#include <dk/assets/CpuData.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <dk/memory/Context.hpp>

struct RenderMemory {
    dk::memory::MemorySystem system = std::move(dk::memory::MemorySystem::create().value());
    dk::memory::ResourceHandle heap = system.create_heap({"render-test", dk::memory::DomainCategory::render}).value();
    dk::memory::ThreadContext context{system};
    dk::memory::ExecutionScope scope{context, heap};
};
inline dk::CpuAsset render_asset() {
    dk::CpuAsset asset;
    asset.mesh.id = dk::AssetId::generate().value();
    for (unsigned n = 0; n < 2; ++n) {
        dk::MeshPrimitive p;
        p.positions = {{-1,0,static_cast<float>(n)}, {1,0,0}, {0,1,0}};
        p.indices = {0,1,2};
        if (n == 1) { p.normals = {{1,0,0}, {0,1,0}, {0,0,1}}; p.texcoords = {{0,0}, {1,0}, {0.5f,1}}; }
        asset.mesh.primitives.push_back(std::move(p));
        dk::TextureData t;
        t.id = dk::AssetId::generate().value(); t.width = 2; t.height = 2;
        t.rgba8.resize(16);
        for (unsigned i = 0; i < 16; ++i) t.rgba8[i] = static_cast<std::byte>(i * 13 + n);
        t.sampler.min_filter = dk::TextureFilter::nearest_mipmap_nearest;
        t.sampler.mag_filter = dk::TextureFilter::nearest;
        t.sampler.wrap_s = dk::TextureWrap::clamp; t.sampler.wrap_t = dk::TextureWrap::mirrored_repeat;
        asset.textures.push_back(std::move(t));
        dk::MaterialData m;
        m.id = dk::AssetId::generate().value(); m.base_color_texture = asset.textures.back().id;
        asset.materials.push_back(m); asset.mesh.primitives.back().material = m.id;
    }
    return asset;
}
