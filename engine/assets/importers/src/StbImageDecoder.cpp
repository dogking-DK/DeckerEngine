#include "StbImageDecoder.hpp"
#include "ImportInternal.hpp"
#include <dk/profiling/Profiler.hpp>
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <stb_image.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#include <cstring>
#include <memory>

namespace dk::import_detail {
TextureData decode_image(std::span<const std::byte> bytes, std::string_view origin, std::string_view mime,
    const ImportLimits& limits, std::size_t& texture_used, std::size_t& output_used)
{
    DK_PROFILE_ZONE("Assets.DecodeImage");
    require(bytes.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()), "Image input exceeds decoder limit");
    constexpr unsigned char png[]{137,80,78,71,13,10,26,10};
    const bool is_png = bytes.size() >= 8 && std::memcmp(bytes.data(),png,8) == 0;
    const bool is_jpeg = bytes.size() >= 3 && bytes[0] == std::byte{0xff} && bytes[1] == std::byte{0xd8} && bytes[2] == std::byte{0xff};
    require(is_png || is_jpeg, "Only PNG/JPEG images supported", ErrorCode::not_supported);
    require(mime.empty() || (is_png && mime == "image/png") || (is_jpeg && mime == "image/jpeg"), "Image MIME disagrees with content");
    const auto* input = reinterpret_cast<const stbi_uc*>(bytes.data()); const auto size = static_cast<int>(bytes.size());
    require(!stbi_is_16_bit_from_memory(input,size), "16-bit images unsupported", ErrorCode::not_supported);
    int width = 0, height = 0, channels = 0;
    require(stbi_info_from_memory(input,size,&width,&height,&channels) != 0, "Invalid image header");
    require(width > 0 && height > 0 && static_cast<std::size_t>(width) <= limits.image_dimension
        && static_cast<std::size_t>(height) <= limits.image_dimension, "Image dimension exceeds budget");
    const auto count = product(static_cast<std::size_t>(width), static_cast<std::size_t>(height), limits.image_bytes / 4, "image pixels") * 4;
    consume(texture_used,count,limits.texture_bytes,"decoded textures"); consume(output_used,count,limits.output_bytes,"CPU output");
    int decoded_width = 0, decoded_height = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels{stbi_load_from_memory(input,size,&decoded_width,&decoded_height,&channels,4),stbi_image_free};
    require(pixels != nullptr, "Invalid image payload"); require(width == decoded_width && height == decoded_height, "Image dimensions changed");
    TextureData result; result.width = static_cast<std::uint32_t>(width); result.height = static_cast<std::uint32_t>(height);
    result.origin = owned(origin); const auto* begin = reinterpret_cast<const std::byte*>(pixels.get()); result.rgba8.assign(begin,begin+count); return result;
}
} // namespace dk::import_detail
