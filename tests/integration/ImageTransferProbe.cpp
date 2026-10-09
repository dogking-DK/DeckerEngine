#include <dk/graphics/Transfer.hpp>
#include <dk/memory/MemorySystem.hpp>
#include "SubmissionInternal.hpp"
#include <array>
#include <atomic>
#include <cstdio>
#include <stdexcept>

using namespace dk;
using namespace dk::graphics;
namespace {
template<class T,class E> T take(std::expected<T,E> r) {
    if (!r) { if constexpr (std::is_same_v<E,Error>) throw std::runtime_error(r.error().message); else throw std::bad_alloc{}; }
    return std::move(*r);
}
void check(Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }
void require(bool v,const char* message) { if (!v) throw std::runtime_error(message); }
struct Diagnostics { std::atomic<unsigned> errors=0,warnings=0,known=0; };
void diagnostic(void* context,const Diagnostic& m) noexcept {
    auto& d=*static_cast<Diagnostics*>(context);
    if (m.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++d.errors;
    if (m.severity==VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        ++d.warnings;
        if (m.name=="Loader Message" && m.message=="Layer VK_LAYER_AMD_switchable_graphics uses API version 1.3 which is older than the application specified API version of 1.4. May cause issues.") ++d.known;
    }
    if (m.severity>=VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
        std::fprintf(stderr,"%.*s: %.*s\n",int(m.name.size()),m.name.data(),int(m.message.size()),m.message.data());
}
PFN_vkQueueSubmit2 native_submit=nullptr;
bool fail=false;
VKAPI_ATTR VkResult VKAPI_CALL submit(VkQueue queue,std::uint32_t count,const VkSubmitInfo2* info,VkFence fence) {
    if (std::exchange(fail,false)) return VK_ERROR_OUT_OF_HOST_MEMORY;
    return native_submit(queue,count,info,fence);
}
}
int main() {
    Diagnostics diagnostics;
    auto memory=take(memory::MemorySystem::create());
    auto heap=take(memory.create_heap({"image-transfer",memory::DomainCategory::render}));
    try {
        {
            DeviceOptions options; options.validation=ValidationMode::required; options.secondary_queue=true;
            options.diagnostic_sink=diagnostic; options.diagnostic_user_data=&diagnostics;
            auto created=Device::create(heap,options);
            if (!created) {
                std::fprintf(stderr,"%s\n",created.error().message.c_str());
                return created.error().code==ErrorCode::not_found || created.error().code==ErrorCode::not_supported ? 77 : 1;
            }
            auto device=std::move(*created);
            if (device.queue_count()<2) return 77;
            native_submit=reinterpret_cast<PFN_vkQueueSubmit2>(device.device_proc("vkQueueSubmit2"));
            auto consumer=take(graphics::detail::SubmissionAccess::create(heap,take(device.share_queue(0)),3,{submit}));
            auto foreign=take(SubmissionQueue::create(heap,take(Device::create(heap,options))));
            for (unsigned round=0;round<3;++round) {
                Image imported;
                std::array<std::byte,16*16*4> expected{},actual{};
                for (std::size_t i=0;i<expected.size();++i) expected[i]=std::byte((i*13+round*31)&255);
                {
                    auto producer=take(SubmissionQueue::create(heap,take(device.share_queue(1))));
                    auto source=take(producer.create_image({16,16}));
                    require(!producer.export_image(source),"uninitialized image exported");
                    auto batch=take(producer.begin());
                    check(batch.upload(source,expected,{0,0,0,0,16,16}));
                    check(batch.transition(source,vk::ImageLayout::eShaderReadOnlyOptimal));
                    require(!producer.export_image(source),"recording image exported");
                    auto completion=take(producer.submit(std::move(batch)));
                    require(!producer.export_image(source),"uncollected image exported");
                    require(take(producer.wait(completion)),"producer wait failed");
                    auto transfer=take(producer.export_image(source));
                    require(!producer.export_image(source),"sealed image exported twice");
                    { auto rejected=take(producer.begin()); require(!rejected.retain(source),"sealed image reused"); }
                    require(!foreign.import_image(std::move(transfer)) && bool(transfer),"foreign import consumed transfer");
                    imported=take(consumer.import_image(std::move(transfer)));
                    require(!transfer && !consumer.import_image(std::move(transfer)),"transfer consumed twice");
                    check(producer.close());
                } // Producer queue, pools and source handles are gone before consumer submits.
                const auto before=take(imported.state());
                { auto batch=take(consumer.begin()); auto readback=take(batch.readback(imported,{0,0,0,0,16,16}));
                  fail=true; require(!consumer.submit(std::move(batch)),"injected submit failure ignored");
                  require(take(imported.state())==before,"failed consumer submit published state"); }
                {
                    auto batch=take(consumer.begin()); auto readback=take(batch.readback(imported,{0,0,0,0,16,16}));
                    auto completion=take(consumer.submit(std::move(batch)));
                    imported={}; // Pending submission retains image and source timeline.
                    require(take(consumer.wait(completion)),"consumer wait failed");
                    require(take(readback.try_read(actual)) && actual==expected,"cross-queue pixels differ");
                }
            }
            check(consumer.close()); check(foreign.close());
            VmaTotalStatistics stats{}; vmaCalculateStatistics(device.allocator(),&stats);
            require(stats.total.statistics.allocationCount==0,"transferred images leaked VMA allocations");
        }
        std::printf("image transfer: 3 rounds, rejected invalid/foreign/repeated handoffs, submit failure, producer-first destruction and pixels passed\n");
        std::printf("validation errors=%u warnings=%u known=%u live=%zu\n",diagnostics.errors.load(),diagnostics.warnings.load(),diagnostics.known.load(),heap.snapshot().live_allocations);
        return !diagnostics.errors && diagnostics.warnings==diagnostics.known && !heap.snapshot().live_allocations && memory.try_close().closed() ? 0 : 1;
    } catch (const std::exception& e) { std::fprintf(stderr,"image transfer failed: %s\n",e.what()); return 1; }
}
