#include "AccelerationPolicy.hpp"
#include <catch2/catch_test_macros.hpp>

using namespace dk::graphics;
namespace policy = dk::graphics::detail;
TEST_CASE("AS counts reject empty device limits and integer truncation")
{
    REQUIRE(policy::acceleration_count(1, 1));
    REQUIRE(policy::acceleration_count(UINT32_MAX, UINT64_MAX));
    REQUIRE_FALSE(policy::acceleration_count(0, 100));
    REQUIRE_FALSE(policy::acceleration_count(101, 100));
    REQUIRE_FALSE(policy::acceleration_count(std::uint64_t{UINT32_MAX}+1, UINT64_MAX));
}
TEST_CASE("AS addresses and scratch alignment reject overflow and short ranges")
{
    REQUIRE(*policy::acceleration_address(4096, 4, 12, 4) == 4100);
    REQUIRE_FALSE(policy::acceleration_address(0, 0, 12, 4));
    REQUIRE_FALSE(policy::acceleration_address(4096, 2, 12, 4));
    REQUIRE_FALSE(policy::acceleration_address(UINT64_MAX-3, 4, 4, 4));
    REQUIRE_FALSE(policy::acceleration_address(UINT64_MAX-3, 0, 8, 4));
    REQUIRE(*policy::scratch_capacity(1024, 128) == 1151);
    REQUIRE(*policy::scratch_capacity(0, 128) == 0);
    REQUIRE_FALSE(policy::scratch_capacity(UINT64_MAX, 128));
    REQUIRE_FALSE(policy::scratch_capacity(1024, 0));
    REQUIRE_FALSE(policy::scratch_capacity(1024, 3));
    REQUIRE(*policy::scratch_address(4097, 1151, 1024, 128) == 4224);
    REQUIRE(*policy::scratch_address(4096, 1024, 1024, 128) == 4096);
    REQUIRE_FALSE(policy::scratch_address(4097, 1150, 1024, 128));
    REQUIRE_FALSE(policy::scratch_address(UINT64_MAX-1, 1151, 1024, 128));
}
TEST_CASE("AS triangles validate packed interleaved and indexed input bounds")
{
    TriangleGeometry geometry; geometry.vertex_count=3; geometry.primitive_count=1;
    BufferDesc vertices{36, policy::acceleration_input_usage}, indices{6, policy::acceleration_input_usage};
    REQUIRE(policy::triangle_description(geometry, vertices, nullptr, 1));
    vertices.size=35; REQUIRE_FALSE(policy::triangle_description(geometry, vertices, nullptr, 1));
    vertices.size=80; geometry.vertex_stride=32; geometry.vertex_offset=4;
    REQUIRE(policy::triangle_description(geometry, vertices, nullptr, 1));
    geometry.vertex_offset=8; REQUIRE_FALSE(policy::triangle_description(geometry, vertices, nullptr, 1));
    geometry.vertex_offset=4; geometry.vertex_stride=31; REQUIRE_FALSE(policy::triangle_description(geometry, vertices, nullptr, 1));
    geometry.vertex_stride=std::uint64_t{UINT32_MAX}+1; REQUIRE_FALSE(policy::triangle_description(geometry, vertices, nullptr, 1));
    geometry.vertex_stride=12; geometry.vertex_offset=0; geometry.index_type=vk::IndexType::eUint16;
    REQUIRE(policy::triangle_description(geometry, vertices, &indices, 1));
    geometry.index_type=vk::IndexType::eUint32; REQUIRE_FALSE(policy::triangle_description(geometry, vertices, &indices, 1));
    indices.size=16; geometry.index_offset=4; REQUIRE(policy::triangle_description(geometry, vertices, &indices, 1));
    geometry.index_offset=2; REQUIRE_FALSE(policy::triangle_description(geometry, vertices, &indices, 1));
    geometry.index_offset=0; geometry.index_type=vk::IndexType::eUint8; REQUIRE_FALSE(policy::triangle_description(geometry, vertices, &indices, 1));
    geometry.primitive_count=2; REQUIRE_FALSE(policy::triangle_description(geometry, vertices, nullptr, 2));
    geometry.primitive_count=1; geometry.vertex_count=0; REQUIRE_FALSE(policy::triangle_description(geometry, vertices, nullptr, 1));
}
TEST_CASE("AS triangle format usage and huge byte ranges are checked before native queries")
{
    TriangleGeometry geometry; geometry.vertex_count=3; geometry.primitive_count=1;
    BufferDesc vertices{36, policy::acceleration_input_usage};
    geometry.vertex_format=vk::Format::eR8G8B8A8Unorm; REQUIRE_FALSE(policy::triangle_description(geometry, vertices, nullptr, 1));
    geometry.vertex_format=vk::Format::eR32G32B32Sfloat; vertices.usage=vk::BufferUsageFlagBits::eVertexBuffer;
    REQUIRE_FALSE(policy::triangle_description(geometry, vertices, nullptr, 1));
    vertices.usage=policy::acceleration_input_usage; geometry.vertex_offset=UINT64_MAX-3;
    REQUIRE_FALSE(policy::triangle_description(geometry, vertices, nullptr, 1));
    geometry.vertex_offset=0; geometry.vertex_count=UINT32_MAX; geometry.vertex_stride=UINT32_MAX-3;
    REQUIRE_FALSE(policy::triangle_description(geometry, vertices, nullptr, 1));
    vertices.size=UINT64_MAX; REQUIRE(policy::triangle_description(geometry, vertices, nullptr, 1));
}
TEST_CASE("AS instances reject invalid transforms and packed field truncation")
{
    AccelerationStructureInstance instance;
    REQUIRE(policy::instance_description(instance));
    instance.transform[0]=-2; instance.transform[3]=100;
    instance.custom_index=instance.sbt_record_offset=0xffffff; instance.mask=0xff;
    REQUIRE(policy::instance_description(instance));
    instance.custom_index=0x1000000; REQUIRE_FALSE(policy::instance_description(instance));
    instance.custom_index=0; instance.sbt_record_offset=0x1000000; REQUIRE_FALSE(policy::instance_description(instance));
    instance.sbt_record_offset=0; instance.mask=256; REQUIRE_FALSE(policy::instance_description(instance));
    instance.mask=255; instance.transform[0]=0; REQUIRE_FALSE(policy::instance_description(instance));
    instance.transform[0]=1; instance.transform[11]=std::numeric_limits<float>::infinity(); REQUIRE_FALSE(policy::instance_description(instance));
    instance.transform[11]=std::numeric_limits<float>::quiet_NaN(); REQUIRE_FALSE(policy::instance_description(instance));
    instance.transform[11]=0; instance.flags=vk::GeometryInstanceFlagBitsKHR::eForceOpaque|vk::GeometryInstanceFlagBitsKHR::eForceNoOpaque;
    REQUIRE_FALSE(policy::instance_description(instance));
    instance.flags=vk::GeometryInstanceFlagsKHR{0x80}; REQUIRE_FALSE(policy::instance_description(instance));
    instance.flags=vk::GeometryInstanceFlagBitsKHR::eTriangleFacingCullDisable; REQUIRE(policy::instance_description(instance));
    REQUIRE_FALSE(AccelerationStructure{}.state()); REQUIRE_FALSE(AccelerationStructure{}.device_address());
}
