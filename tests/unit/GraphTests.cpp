#include <dk/graphics/Graph.hpp>
#include <dk/memory/MemorySystem.hpp>
#include <catch2/catch_test_macros.hpp>
#include <array>
#include <limits>
#include <type_traits>

using namespace dk::graphics;
using namespace dk::graphics::graph;
namespace {
struct Fixture {
    dk::memory::MemorySystem system = std::move(dk::memory::MemorySystem::create().value());
    dk::memory::ResourceHandle heap;
    Graph graph;
    explicit Fixture(std::size_t budget = 0) {
        heap = system.create_heap({"graph-tests", dk::memory::DomainCategory::render, budget}).value();
        graph = std::move(Graph::create(heap).value());
    }
};
template<class T> T take(dk::Result<T> result) {
    if (!result) { INFO(result.error().message); FAIL("unexpected graph error"); }
    return std::move(*result);
}
Use buffer_use(BufferId id, vk::AccessFlags2 access, vk::PipelineStageFlags2 stages = vk::PipelineStageFlagBits2::eCopy,
               bool overwrite = false, vk::DeviceSize offset = 0, vk::DeviceSize size = VK_WHOLE_SIZE) {
    return {id, {{stages, access}, offset, size, {}, overwrite}};
}
Use image_use(ImageId id, vk::AccessFlags2 access, vk::ImageLayout layout,
              vk::PipelineStageFlags2 stages = vk::PipelineStageFlagBits2::eCopy, bool overwrite = false,
              vk::ImageSubresourceRange range = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1}) {
    return {id, {{stages, access, layout}, 0, VK_WHOLE_SIZE, range, overwrite}};
}
PassId add(Graph& graph, std::string_view name, std::initializer_list<Use> uses, bool side_effect = false) {
    return take(graph.add_pass({name, {uses.begin(), uses.size()}, side_effect}));
}
void check_valid(const Graph& graph) {
    const auto result = graph.validate();
    if (!result) {
        INFO(result.error().message);
        for (const auto& context : result.error().context) INFO(context);
        FAIL("expected valid graph");
    }
}
constexpr auto transfer_read = vk::AccessFlagBits2::eTransferRead;
constexpr auto transfer_write = vk::AccessFlagBits2::eTransferWrite;
constexpr auto src_layout = vk::ImageLayout::eTransferSrcOptimal;
constexpr auto dst_layout = vk::ImageLayout::eTransferDstOptimal;
static_assert(!std::is_copy_constructible_v<Graph>);
static_assert(!std::is_convertible_v<BufferId, ImageId>);
static_assert(!std::is_convertible_v<PassId, BufferId>);
}

TEST_CASE("graph declares upload compute draw and readback without a device") {
    Fixture f;
    auto& g = f.graph;
    const auto upload = take(g.declare_buffer("upload", {64, vk::BufferUsageFlagBits::eTransferSrc, BufferMemory::upload}, Lifetime::external, true));
    const auto mesh = take(g.declare_buffer("mesh", {64, vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eVertexBuffer}));
    const auto color = take(g.declare_image("color", {16, 16, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferSrc}));
    const auto readback = take(g.declare_buffer("readback", {1024, vk::BufferUsageFlagBits::eTransferDst, BufferMemory::readback}, Lifetime::external));
    const auto copy = add(g, "upload", {buffer_use(upload, transfer_read), buffer_use(mesh, transfer_write, vk::PipelineStageFlagBits2::eCopy, true)});
    add(g, "compute", {buffer_use(mesh, vk::AccessFlagBits2::eShaderStorageRead | vk::AccessFlagBits2::eShaderStorageWrite, vk::PipelineStageFlagBits2::eComputeShader)});
    add(g, "draw", {buffer_use(mesh, vk::AccessFlagBits2::eVertexAttributeRead, vk::PipelineStageFlagBits2::eVertexAttributeInput),
        image_use(color, vk::AccessFlagBits2::eColorAttachmentWrite, vk::ImageLayout::eColorAttachmentOptimal, vk::PipelineStageFlagBits2::eColorAttachmentOutput, true)});
    const auto download = add(g, "readback", {image_use(color, transfer_read, src_layout), buffer_use(readback, transfer_write, vk::PipelineStageFlagBits2::eCopy, true)}, true);
    REQUIRE(g.mark_output(readback));
    check_valid(g);
    CHECK(g.counts() == Counts{3, 1, 4, 0, 1});
    CHECK(take(g.pass(copy)).uses[0].access.size == 64);
    CHECK(take(g.pass(download)).side_effect);
    CHECK(take(g.buffer(upload)).lifetime == Lifetime::external);
    CHECK(take(g.image(color)).description.width == 16);
}

TEST_CASE("graph owns declaration inputs and empty graphs are valid") {
    Fixture f;
    check_valid(f.graph);
    std::string name = "owned name";
    const auto buffer = take(f.graph.declare_buffer(name, {64}, Lifetime::external, true));
    std::array uses{buffer_use(buffer, transfer_read)};
    const auto pass = take(f.graph.add_pass({name, uses}));
    name.assign("changed"); uses[0].access.size = 1;
    CHECK(take(f.graph.buffer(buffer)).name == "owned name");
    CHECK(take(f.graph.pass(pass)).name == "owned name");
    CHECK(take(f.graph.pass(pass)).uses[0].access.size == 64);
    CHECK(take(f.graph.pass(add(f.graph, "side effect", {}, true))).side_effect);
    check_valid(f.graph);
}

TEST_CASE("graph rejects invalid descriptions names and duplicate declarations atomically") {
    Fixture f;
    auto& g = f.graph;
    CHECK_FALSE(g.declare_buffer("", {64}));
    CHECK_FALSE(g.declare_buffer(std::string_view{"bad\0name", 8}, {64}));
    CHECK_FALSE(g.declare_buffer("zero", {}));
    CHECK_FALSE(g.declare_buffer("bad lifetime", {64}, static_cast<Lifetime>(99)));
    CHECK_FALSE(g.declare_buffer("transient initialized", {64}, Lifetime::transient, true));
    CHECK_FALSE(g.declare_image("zero", {}));
    CHECK_FALSE(g.declare_image("too many mips", {4, 4, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eSampled, 4}));
    const auto id = take(g.declare_buffer("same", {64}));
    CHECK_FALSE(g.declare_buffer("same", {128}));
    CHECK_FALSE(g.declare_image("same", {4, 4}));
    CHECK(take(g.buffer(id)).description.size == 64);
    add(g, "pass", {});
    CHECK_FALSE(g.add_pass({"pass", {}}));
    CHECK_FALSE(g.add_pass({"", {}}));
    CHECK(g.counts() == Counts{1, 0, 1, 0, 0});
}

TEST_CASE("graph rejects invalid expired foreign and replaced handles") {
    Fixture f;
    auto other = take(Graph::create(f.heap));
    const auto a = take(f.graph.declare_buffer("a", {64}, Lifetime::external, true));
    const auto p = add(f.graph, "p", {});
    CHECK_FALSE(other.buffer(a));
    CHECK_FALSE(other.mark_output(a));
    const std::array uses{buffer_use(a, transfer_read)};
    CHECK_FALSE(other.add_pass({"foreign", uses}));
    CHECK_FALSE(f.graph.buffer({}));
    CHECK_FALSE(f.graph.image({}));
    CHECK_FALSE(f.graph.pass({}));
    CHECK_FALSE(f.graph.add_dependency(p, {}));
    CHECK_FALSE(f.graph.remove_dependency({}, p));
    auto moved = std::move(f.graph);
    CHECK_FALSE(f.graph.validate());
    CHECK(moved.buffer(a));
    CHECK(moved.pass(p));
    const auto old = take(other.declare_buffer("old", {4}));
    other = std::move(moved);
    CHECK_FALSE(old);
    CHECK(other.buffer(a));
    REQUIRE(other.reset());
    CHECK_FALSE(a); CHECK_FALSE(p);
    CHECK_FALSE(other.buffer(a));
    const auto new_id = take(other.declare_buffer("new", {64}));
    CHECK(new_id != a);
    CHECK(other.counts().buffers == 1);
    ImageId dead;
    { auto temporary = take(Graph::create(f.heap)); dead = take(temporary.declare_image("temp", {1, 1})); }
    CHECK_FALSE(dead);
    CHECK_FALSE(other.mark_output(dead));
}

TEST_CASE("graph validates buffer ranges state and usage before adding a Pass") {
    Fixture f;
    const auto id = take(f.graph.declare_buffer("buffer", {64}, Lifetime::external, true));
    const auto valid = buffer_use(id, transfer_read);
    const auto prefix = take(f.graph.declare_buffer("prefix", {64}, Lifetime::external, true));
    auto reject = [&](Use use) {
        const std::array uses{buffer_use(prefix, transfer_read), use}; // Failure after processing a valid candidate still publishes nothing.
        REQUIRE_FALSE(f.graph.add_pass({"bad", uses}));
        CHECK(f.graph.counts().passes == 0);
    };
    auto use = valid; use.access.offset = 65; reject(use);
    use = valid; use.access.size = 0; reject(use);
    use = valid; use.access.offset = 63; use.access.size = 2; reject(use);
    use = valid; use.access.offset = std::numeric_limits<vk::DeviceSize>::max(); use.access.size = 4; reject(use);
    use = valid; use.access.state.stages = {}; reject(use);
    use = valid; use.access.state.stages = vk::PipelineStageFlags2{1ull << 63}; reject(use);
    use = valid; use.access.state.stages = vk::PipelineStageFlagBits2::eFragmentShader; reject(use);
    use = valid; use.access.state.access = {}; reject(use);
    use = valid; use.access.state.access = vk::AccessFlags2{1ull << 63}; reject(use);
    use = valid; use.access.state.layout = src_layout; reject(use);
    use = valid; use.access.state.initialized = true; reject(use);
    use = valid; use.access.state = {vk::PipelineStageFlagBits2::eHost, vk::AccessFlagBits2::eHostRead}; reject(use);
    use = buffer_use(id, vk::AccessFlagBits2::eShaderStorageRead, vk::PipelineStageFlagBits2::eComputeShader); reject(use);
    use = valid; use.access.full_overwrite = true; reject(use);
    CHECK(take(f.graph.pass(add(f.graph, "slice", {buffer_use(id, transfer_read, vk::PipelineStageFlagBits2::eCopy, false, 60)}))).uses[0].access.size == 4);
}

TEST_CASE("graph validates image aspects subresources layouts and usage") {
    Fixture f;
    const auto image = take(f.graph.declare_image("image", {8, 8, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eTransferSrc, 4, 2}, Lifetime::external, true));
    const auto valid = image_use(image, transfer_read, src_layout);
    auto reject = [&](Use use) {
        const std::array uses{use};
        CHECK_FALSE(f.graph.add_pass({"invalid", uses}));
        CHECK(f.graph.counts().passes == 0);
    };
    auto use = valid; use.access.range.aspectMask = vk::ImageAspectFlagBits::eDepth; reject(use);
    use = valid; use.access.range.levelCount = 0; reject(use);
    use = valid; use.access.range.baseMipLevel = 4; reject(use);
    use = valid; use.access.range.layerCount = VK_REMAINING_ARRAY_LAYERS; reject(use);
    use = valid; use.access.range.baseArrayLayer = 2; reject(use);
    use = valid; use.access.state.layout = vk::ImageLayout::eUndefined; reject(use);
    use = valid; use.access.state.layout = dst_layout; reject(use);
    use = valid; use.access.state.access = transfer_write; reject(use);
    use = valid; use.access.state = {vk::PipelineStageFlagBits2::eComputeShader, vk::AccessFlagBits2::eShaderStorageRead, vk::ImageLayout::eGeneral}; reject(use);
    add(f.graph, "valid", {valid});
    check_valid(f.graph);
}

TEST_CASE("graph requires merged buffer and overlapping image uses within a Pass") {
    Fixture f;
    const auto b = take(f.graph.declare_buffer("b", {64}, Lifetime::external, true));
    const auto i = take(f.graph.declare_image("i", {8, 8, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eTransferSrc, 2, 1}, Lifetime::external, true));
    const std::array buffers{buffer_use(b, transfer_read, vk::PipelineStageFlagBits2::eCopy, false, 0, 32), buffer_use(b, transfer_read, vk::PipelineStageFlagBits2::eCopy, false, 32, 32)};
    CHECK_FALSE(f.graph.add_pass({"buffer duplicates", buffers}));
    auto read = image_use(i, transfer_read, src_layout);
    std::array images{read, read};
    CHECK_FALSE(f.graph.add_pass({"image overlap", images}));
    images[1].access.range.baseMipLevel = 1;
    REQUIRE(f.graph.add_pass({"separate mips", images}));
    check_valid(f.graph);
}

TEST_CASE("graph rejects undefined reads preserved writes and incomplete outputs") {
    Fixture f;
    const auto b = take(f.graph.declare_buffer("undefined", {64}));
    SECTION("read") { add(f.graph, "reader", {buffer_use(b, transfer_read)}); }
    SECTION("preserved write") { add(f.graph, "writer", {buffer_use(b, transfer_write)}); }
    SECTION("output") { REQUIRE(f.graph.mark_output(b)); }
    const auto counts = f.graph.counts();
    const auto result = f.graph.validate();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == dk::ErrorCode::invalid_argument);
    REQUIRE_FALSE(result.error().context.empty());
    CHECK(result.error().context[0].find("undefined") != std::string::npos);
    CHECK(f.graph.counts() == counts);
}

TEST_CASE("graph combines written byte intervals but rejects gaps") {
    Fixture f;
    const auto b = take(f.graph.declare_buffer("buffer", {64}));
    add(f.graph, "left", {buffer_use(b, transfer_write, vk::PipelineStageFlagBits2::eCopy, true, 0, 32)});
    SECTION("adjacent writes cover a full output") {
        add(f.graph, "right", {buffer_use(b, transfer_write, vk::PipelineStageFlagBits2::eCopy, true, 32, 32)});
        add(f.graph, "read", {buffer_use(b, transfer_read)});
        REQUIRE(f.graph.mark_output(b)); check_valid(f.graph);
    }
    SECTION("gap remains undefined") {
        add(f.graph, "right", {buffer_use(b, transfer_write, vk::PipelineStageFlagBits2::eCopy, true, 33, 31)});
        add(f.graph, "read", {buffer_use(b, transfer_read)});
        CHECK_FALSE(f.graph.validate());
    }
    SECTION("read initialized slice only") {
        add(f.graph, "read", {buffer_use(b, transfer_read, vk::PipelineStageFlagBits2::eCopy, false, 4, 20)});
        check_valid(f.graph);
        REQUIRE(f.graph.mark_output(b)); CHECK_FALSE(f.graph.validate());
    }
}

TEST_CASE("graph tracks mip layer coverage and depth writes") {
    Fixture f;
    const auto i = take(f.graph.declare_image("array", {4, 4, vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, 2, 2}));
    for (std::uint32_t mip = 0; mip < 2; ++mip) {
        for (std::uint32_t layer = 0; layer < 2; ++layer) {
            add(f.graph, "write" + std::to_string(mip * 2 + layer), {image_use(i, transfer_write, dst_layout, vk::PipelineStageFlagBits2::eCopy, true,
                {vk::ImageAspectFlagBits::eColor, mip, 1, layer, 1})});
        }
    }
    add(f.graph, "read all", {image_use(i, transfer_read, src_layout, vk::PipelineStageFlagBits2::eCopy, false, {vk::ImageAspectFlagBits::eColor, 0, 2, 0, 2})});
    REQUIRE(f.graph.mark_output(i));
    const auto depth = take(f.graph.declare_image("depth", {4, 4, vk::Format::eD32Sfloat, vk::ImageUsageFlagBits::eDepthStencilAttachment}));
    add(f.graph, "clear depth", {image_use(depth, vk::AccessFlagBits2::eDepthStencilAttachmentWrite, vk::ImageLayout::eDepthStencilAttachmentOptimal,
        vk::PipelineStageFlagBits2::eEarlyFragmentTests, true, {vk::ImageAspectFlagBits::eDepth, 0, 1, 0, 1})});
    REQUIRE(f.graph.mark_output(depth)); check_valid(f.graph);
}

TEST_CASE("graph reports explicit cycles and permits dependency repair") {
    Fixture f;
    const auto a = add(f.graph, "A", {}), b = add(f.graph, "B", {}), c = add(f.graph, "C", {});
    add(f.graph, "unrelated", {});
    CHECK_FALSE(f.graph.add_dependency(a, a));
    REQUIRE(f.graph.add_dependency(a, b)); REQUIRE(f.graph.add_dependency(a, b));
    REQUIRE(f.graph.add_dependency(b, c)); REQUIRE(f.graph.add_dependency(c, a));
    CHECK(f.graph.counts().dependencies == 3);
    const auto result = f.graph.validate();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == dk::ErrorCode::conflict);
    CHECK(result.error().message.find("'A' -> pass[1] 'B' -> pass[2] 'C' -> pass[0] 'A'") != std::string::npos);
    REQUIRE(f.graph.remove_dependency(c, a)); REQUIRE(f.graph.remove_dependency(c, a));
    CHECK(f.graph.counts().dependencies == 2);
    check_valid(f.graph);
}

TEST_CASE("graph detects RAW WAR and discard WAW cycles") {
    Fixture f;
    const auto b = take(f.graph.declare_buffer("buffer", {64}, Lifetime::external, true));
    Use first = buffer_use(b, transfer_write, vk::PipelineStageFlagBits2::eCopy, true);
    Use second = buffer_use(b, transfer_read);
    SECTION("RAW") {}
    SECTION("WAR") { std::swap(first, second); }
    SECTION("WAW despite discard") { second = first; }
    const auto a = add(f.graph, "first", {first}), z = add(f.graph, "second", {second});
    check_valid(f.graph);
    REQUIRE(f.graph.add_dependency(z, a));
    const auto result = f.graph.validate();
    REQUIRE_FALSE(result);
    CHECK(result.error().code == dk::ErrorCode::conflict);
}

TEST_CASE("graph treats image layout changes as dependencies and disjoint mips independently") {
    Fixture f;
    const auto i = take(f.graph.declare_image("image", {8, 8, vk::Format::eR8G8B8A8Unorm, vk::ImageUsageFlagBits::eTransferSrc, 2}, Lifetime::external, true));
    auto use = image_use(i, transfer_read, src_layout);
    const auto a = add(f.graph, "A", {use});
    SECTION("read read same layout can reverse") {
        const auto b = add(f.graph, "B", {use});
        REQUIRE(f.graph.add_dependency(b, a)); check_valid(f.graph);
    }
    SECTION("read read layout transition cannot reverse") {
        use.access.state.layout = vk::ImageLayout::eGeneral;
        const auto b = add(f.graph, "B", {use});
        REQUIRE(f.graph.add_dependency(b, a)); CHECK_FALSE(f.graph.validate());
    }
    SECTION("separate mips can reverse with different layouts") {
        use.access.range.baseMipLevel = 1; use.access.state.layout = vk::ImageLayout::eGeneral;
        const auto b = add(f.graph, "B", {use});
        REQUIRE(f.graph.add_dependency(b, a)); check_valid(f.graph);
    }
}

TEST_CASE("graph output marking is idempotent and validation sees later mutations") {
    Fixture f;
    const auto b = take(f.graph.declare_buffer("external", {64}, Lifetime::external, true));
    REQUIRE(f.graph.mark_output(b)); REQUIRE(f.graph.mark_output(b));
    CHECK(f.graph.counts().outputs == 1); check_valid(f.graph);
    const auto pending = take(f.graph.declare_image("undefined image", {4, 4}));
    REQUIRE(f.graph.mark_output(pending)); CHECK_FALSE(f.graph.validate());
    add(f.graph, "initialize", {image_use(pending, transfer_write, dst_layout, vk::PipelineStageFlagBits2::eCopy, true)});
    check_valid(f.graph);
}

TEST_CASE("graph budget failures preserve declarations handles and reset identity") {
    Fixture f{16384};
    const auto b = take(f.graph.declare_buffer("b", {64}, Lifetime::external, true));
    const auto a = add(f.graph, "a", {}), z = add(f.graph, "z", {});
    const auto original = f.graph.counts();
    const std::string huge(32768, 'x');
    CHECK_FALSE(f.graph.declare_buffer(huge, {4}));
    CHECK_FALSE(f.graph.declare_image(huge, {4, 4}));
    CHECK_FALSE(f.graph.add_pass({huge, {}}));
    CHECK(f.graph.counts() == original);
    std::vector<void*> padding;
    while (auto block = f.heap.try_allocate(1)) padding.push_back(*block);
    CHECK_FALSE(f.graph.reset());
    CHECK_FALSE(f.graph.add_dependency(a, z));
    CHECK_FALSE(f.graph.mark_output(b));
    CHECK_FALSE(f.graph.validate());
    CHECK(f.graph.counts() == original);
    CHECK(f.graph.buffer(b));
    for (void* block : padding) f.heap.deallocate(block, 1);
    check_valid(f.graph);
    REQUIRE(f.graph.add_dependency(a, z));
    REQUIRE(f.graph.mark_output(b));
    REQUIRE(f.graph.reset());
    CHECK_FALSE(b); CHECK_FALSE(a);
}

TEST_CASE("graph closing domains reject operations and release all allocations") {
    Fixture f;
    auto b = take(f.graph.declare_buffer("b", {64}));
    auto p = add(f.graph, "p", {});
    f.heap.begin_close();
    CHECK_FALSE(f.graph.validate()); CHECK_FALSE(f.graph.reset());
    CHECK_FALSE(f.graph.declare_buffer("x", {64}));
    CHECK_FALSE(f.graph.add_pass({"x", {}}));
    CHECK_FALSE(f.graph.mark_output(b));
    CHECK_FALSE(f.graph.buffer(b));
    CHECK_FALSE(f.graph.add_dependency(p, p));
    CHECK_FALSE(Graph::create(f.heap));
    CHECK_FALSE(Graph::create({}));
    f.graph = Graph{};
    CHECK_FALSE(b); CHECK_FALSE(p);
    b = {}; p = {}; // Weak IDs keep only the domain-allocated identity control block.
    CHECK(f.heap.snapshot().live_allocations == 0);
    CHECK(f.heap.try_close().closed());
}

TEST_CASE("graph rejects partially initialized mip and layer reads") {
    Fixture f;
    const auto image = take(f.graph.declare_image("partial", {4, 4, vk::Format::eR8G8B8A8Unorm,
        vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst, 2, 2}));
    add(f.graph, "write only first subresource", {image_use(image, transfer_write, dst_layout,
        vk::PipelineStageFlagBits2::eCopy, true)});
    auto read = image_use(image, transfer_read, src_layout);
    SECTION("initialized subresource is readable but whole output is not") {
        add(f.graph, "read initialized", {read}); check_valid(f.graph);
        REQUIRE(f.graph.mark_output(image)); CHECK_FALSE(f.graph.validate());
    }
    SECTION("other mip is not initialized") {
        read.access.range.baseMipLevel = 1;
        add(f.graph, "read other mip", {read}); CHECK_FALSE(f.graph.validate());
    }
    SECTION("other layer is not initialized") {
        read.access.range.baseArrayLayer = 1;
        add(f.graph, "read other layer", {read}); CHECK_FALSE(f.graph.validate());
    }
    SECTION("preserved partial write cannot initialize missing contents") {
        auto write = image_use(image, transfer_write, dst_layout);
        write.access.range.baseArrayLayer = 1;
        add(f.graph, "partial write", {write}); CHECK_FALSE(f.graph.validate());
    }
}

TEST_CASE("graph partial allocation failures unwind construction and publication") {
    auto system = take(dk::memory::MemorySystem::create().transform_error([](auto) {
        return dk::Error{dk::ErrorCode::internal_error, "memory system creation failed"};
    }));
    bool create_failed = false, pass_failed = false, validation_failed = false, completed = false;
    // The optimized build's validation scratch window can be smaller than 31 bytes.
    // Visit every budget so that configuration-dependent object sizes cannot skip it.
    for (std::size_t budget = 1; budget < 4096; ++budget) {
        const auto heap = system.create_heap({"graph-budget-sweep", dk::memory::DomainCategory::render, budget}).value();
        {
            auto graph = Graph::create(heap);
            if (!graph) create_failed = true;
            else {
                const auto before = graph->counts();
                auto resource = graph->declare_buffer(std::string(127, 'r'), {64}, Lifetime::external, true);
                if (!resource) CHECK(graph->counts() == before);
                else {
                    const auto declared = graph->counts();
                    const std::array uses{buffer_use(*resource, transfer_read)};
                    const auto pass = graph->add_pass({std::string(127, 'p'), uses});
                    if (!pass) { pass_failed = true; CHECK(graph->counts() == declared); }
                    const auto added = graph->counts();
                    const auto valid = graph->validate();
                    if (!valid) validation_failed = true;
                    if (pass && valid) completed = true;
                    CHECK(graph->counts() == added);
                }
            }
        }
        CHECK(heap.snapshot().live_allocations == 0);
        CHECK(heap.try_close().closed());
    }
    CHECK(create_failed); CHECK(pass_failed); CHECK(validation_failed); CHECK(completed);
}
