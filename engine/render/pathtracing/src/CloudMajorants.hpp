#pragma once
#include <dk/render/CloudFields.hpp>
#include <algorithm>
#include <cstdint>
#include <span>

namespace dk::render::detail {
// A normalized cell covers [b*edge,(b+1)*edge] in texel coordinates.
// Texel-centred interpolation needs [b*edge-1,(b+1)*edge], including repeat.
inline constexpr std::uint32_t cloud_bound_edge=8;
inline std::uint32_t cloud_bound_size(std::uint32_t size) { return std::max(1u,size/cloud_bound_edge); }
inline std::size_t cloud_bound_bytes(CloudFieldKey key) {
    const auto w=cloud_bound_size(key.weather_resolution),b=cloud_bound_size(key.base_resolution);
    return std::size_t{w}*w*2+std::size_t{b}*b*b;
}
// Appends byte maxima to the existing packed RG8/R8 source buffer. No field mutation.
inline void pack_cloud_bounds(const CloudFieldData& field,std::span<std::uint32_t> output) {
    const auto key=field.key(); std::size_t destination=field.bytes();
    const auto append=[&](std::uint8_t value) {
        output[destination/4]|=std::uint32_t(value)<<((destination%4)*8); ++destination;
    };
    for(const auto kind:{CloudField::weather,CloudField::base}) {
        const bool volume=kind==CloudField::base;
        const auto n=volume ? key.base_resolution : key.weather_resolution;
        const auto coarse=cloud_bound_size(n),edge=n/coarse,channels=volume ? 1u : 2u;
        const auto bytes=field.texels(kind);
        for(unsigned z=0;z<(volume ? coarse : 1u);++z) for(unsigned y=0;y<coarse;++y) for(unsigned x=0;x<coarse;++x)
        for(unsigned c=0;c<channels;++c) {
            std::uint8_t maximum=0;
            for(int dz=volume ? -1 : 0;dz<=(volume ? int(edge) : 0);++dz)
            for(int dy=-1;dy<=int(edge);++dy) for(int dx=-1;dx<=int(edge);++dx) {
                const auto ix=(x*edge+dx)&(n-1),iy=(y*edge+dy)&(n-1),iz=(z*edge+dz)&(n-1);
                const auto index=ix+n*(iy+(volume ? n*iz : 0));
                maximum=std::max(maximum,bytes[index*channels+c]);
            }
            append(maximum);
        }
    }
}
} // namespace dk::render::detail
