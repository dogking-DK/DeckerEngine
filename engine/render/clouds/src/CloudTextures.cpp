#include <dk/render/CloudTextures.hpp>
#include <algorithm>
#include <limits>

namespace dk::render {
using namespace graphics;
namespace {
template<class T> T take(Result<T> value) { if(!value) throw value.error(); return std::move(*value); }
void check(Result<void> value) { if(!value) throw value.error(); }
}
Result<std::unique_ptr<CloudTextureCache>> CloudTextureCache::create(SubmissionQueue& queue,CloudTextureLimits limits) {
    if(!limits.bytes || limits.bytes>32*1024*1024 || !limits.generations || limits.generations>2)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Invalid cloud texture limits"});
    auto identity=queue.create_buffer({4,vk::BufferUsageFlagBits::eTransferSrc});
    if(!identity) return std::unexpected(identity.error());
    return std::unique_ptr<CloudTextureCache>{new CloudTextureCache(limits,std::move(*identity))};
}
Result<std::shared_ptr<const CloudTextures>> CloudTextureCache::prepare(SubmissionQueue& queue,const CloudFieldData& data) {
    try {
        check(queue.poll());
        // Queue checks precede even cache hits. The retained identity avoids a raw owner pointer.
        auto batch=take(queue.begin()); check(batch.retain(identity_));
        if(!data.bytes()) return std::unexpected(Error{ErrorCode::invalid_argument,"Empty cloud field data"});
        if(!queue.stats().pending_slots && queue.stats().recording_slots==1)
            std::erase_if(live_,[&](const auto& live){ return live!=current_ && live.use_count()==1; });
        if(current_ && current_->key()==data.key()) return current_;
        std::size_t occupied=2*data.bytes(), count=1;
        for(const auto& live:live_) { occupied+=live->bytes(); ++count; }
        if(occupied>limits_.bytes || count>limits_.generations)
            return std::unexpected(Error{ErrorCode::invalid_state,"Cloud texture generation or byte budget exceeded"});
        if(current_ && current_->generation()==std::numeric_limits<std::uint64_t>::max())
            return std::unexpected(Error{ErrorCode::invalid_state,"Cloud texture generation exhausted"});
        auto candidate=std::make_shared<CloudTextures>();
        candidate->key_=data.key(); candidate->hash_=data.hash(); candidate->bytes_=data.bytes();
        candidate->generation_=current_ ? current_->generation()+1 : 1;
        // Reserve tracking storage before any submission; publication cannot allocate.
        live_.reserve(live_.size()+1);
        const std::array sizes{data.key().weather_resolution,data.key().base_resolution,data.key().detail_resolution};
        const auto factory=queue.resources();
        for(unsigned i=0;i<3;++i) {
            ImageDesc desc{sizes[i],sizes[i],i==0 ? vk::Format::eR8G8Unorm : vk::Format::eR8Unorm};
            desc.type=i==0 ? vk::ImageType::e2D : vk::ImageType::e3D; desc.depth=i==0 ? 1 : sizes[i];
            desc.usage=vk::ImageUsageFlagBits::eSampled|vk::ImageUsageFlagBits::eTransferDst|vk::ImageUsageFlagBits::eTransferSrc;
            desc.require_linear_filter=true;
            candidate->images_[i]=take(queue.create_image(desc));
            candidate->views_[i]=take(factory.create_view(candidate->images_[i],{i==0 ? vk::ImageViewType::e2D : vk::ImageViewType::e3D}));
            check(batch.upload(candidate->images_[i],std::as_bytes(data.texels(static_cast<CloudField>(i))),{0,0,0,0,sizes[i],sizes[i],0,0,0,0,desc.depth}));
        }
        SamplerDesc sampler; sampler.address_u=sampler.address_v=sampler.address_w=vk::SamplerAddressMode::eRepeat;
        candidate->sampler_=take(factory.create_sampler(sampler));
        std::array<ResourceUse,3> uses;
        for(unsigned i=0;i<3;++i) uses[i]=image_use(candidate->images_[i],vk::PipelineStageFlagBits2::eComputeShader|vk::PipelineStageFlagBits2::eFragmentShader,
            vk::AccessFlagBits2::eShaderSampledRead,vk::ImageLayout::eShaderReadOnlyOptimal);
        check(batch.prepare(uses));
        const auto submission=take(queue.submit(std::move(batch)));
        if(!take(queue.wait(submission))) return std::unexpected(Error{ErrorCode::invalid_state,"Cloud upload did not complete"});
        live_.push_back(candidate); current_=std::move(candidate); return current_;
    } catch(const Error& error) { return std::unexpected(error); }
}
} // namespace dk::render
