#pragma once
#include <dk/assets/GltfImporter.hpp>
namespace dk::import_detail {
TextureData decode_image(std::span<const std::byte> bytes, std::string_view origin, std::string_view mime,
    const ImportLimits& limits, std::size_t& texture_used, std::size_t& output_used);
}
