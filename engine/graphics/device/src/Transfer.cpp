#include "CommandInternal.hpp"
#include <dk/graphics/Transfer.hpp>

namespace dk::graphics {
namespace {
Result<void> ready(const std::shared_ptr<detail::BatchState>& batch)
{ return batch ? detail::outside_rendering(*batch) : std::unexpected(Error{ErrorCode::invalid_state,"empty command batch"}); }
// Once a composed operation emits commands, a later failure cannot undo them.
struct TransferGuard {
    detail::BatchState& batch;
    std::size_t requests;
    bool emitted = false, committed = false;
    explicit TransferGuard(detail::BatchState& value) : batch(value), requests(value.requests.size()) {}
    ~TransferGuard() {
        if (committed) return;
        if (emitted) batch.invalid = true;
        while (batch.requests.size() > requests) {
            batch.requests.back()->status = ReadbackStatus::cancelled;
            batch.requests.pop_back();
        }
    }
};
Result<void> check_image_read(detail::BatchState& batch, const std::shared_ptr<detail::ResourceState>& image, const detail::CopyFootprint& copy)
{
    auto* local = batch.find(image);
    const auto index = static_cast<std::size_t>(copy.range.baseArrayLayer)*image->image_desc.mip_levels + copy.range.baseMipLevel;
    if (!(local ? local->states[index] : image->states[index]).initialized)
        return std::unexpected(Error{ErrorCode::invalid_state,"image readback requires initialized subresource content"});
    return {};
}
}
ReadbackStatus ReadbackRequest::status() const noexcept
{
    if (!state_) return ReadbackStatus::cancelled;
    if (detail::ObjectAccess::state(state_->staging)->owner->lost) return ReadbackStatus::device_lost;
    return state_->status;
}
ReadbackDescription ReadbackRequest::description() const noexcept { return state_ ? state_->description : ReadbackDescription{}; }
Result<bool> ReadbackRequest::try_read(std::span<std::byte> destination) const
{
    const auto current = status();
    if (current == ReadbackStatus::cancelled || current == ReadbackStatus::device_lost)
        return std::unexpected(Error{ErrorCode::invalid_state,current == ReadbackStatus::device_lost ? "readback device is lost" : "readback is empty or cancelled"});
    if (destination.size() != state_->description.bytes)
        return std::unexpected(Error{ErrorCode::invalid_argument,"readback destination must match request byte count"});
    if (current != ReadbackStatus::ready) return false;
    if (auto result = state_->staging.read(0,destination); !result) return std::unexpected(result.error());
    return true;
}
Result<void> CommandBatch::upload(const Buffer& destination, std::span<const std::byte> bytes, vk::DeviceSize offset)
{
    if (auto valid = ready(state_); !valid) return valid;
    if (auto valid = state_->check(destination.state_); !valid) return valid;
    if (auto valid = detail::validate_copy(bytes.size(),destination.size(),bytes.size(),0,offset,false); !valid) return valid;
    if (!(destination.state_->buffer_desc.usage & vk::BufferUsageFlagBits::eTransferDst))
        return std::unexpected(Error{ErrorCode::invalid_argument,"upload destination requires transfer usage"});
    auto staging = detail::ObjectAccess::factory(*state_).create_buffer({bytes.size(),vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload});
    if (!staging) return std::unexpected(staging.error());
    if (auto valid = staging->write(0,bytes); !valid) return valid;
    const std::array uses{buffer_use(*staging,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),
        buffer_use(destination,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,offset,bytes.size())};
    TransferGuard guard{*state_};
    if (auto valid = prepare(uses); !valid) return valid;
    guard.emitted = true;
    if (auto valid = copy_buffer(*staging,destination,bytes.size(),0,offset); !valid) return valid;
    guard.committed = true;
    return {};
}
Result<void> CommandBatch::upload(const Image& destination, std::span<const std::byte> bytes, const ImageCopyRegion& region)
{
    if (auto valid = ready(state_); !valid) return valid;
    if (auto valid = state_->check(destination.state_); !valid) return valid;
    auto copy = detail::copy_footprint(destination.description(),region);
    if (!copy) return std::unexpected(copy.error());
    if (bytes.size() != region.buffer_offset+copy->bytes || !(destination.description().usage & vk::ImageUsageFlagBits::eTransferDst))
        return std::unexpected(Error{ErrorCode::invalid_argument,"upload bytes must match image footprint and transfer usage"});
    auto staging = detail::ObjectAccess::factory(*state_).create_buffer({bytes.size(),vk::BufferUsageFlagBits::eTransferSrc,BufferMemory::upload});
    if (!staging) return std::unexpected(staging.error());
    if (auto valid = staging->write(0,bytes); !valid) return valid;
    const std::array uses{buffer_use(*staging,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead),
        image_use(destination,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite,vk::ImageLayout::eTransferDstOptimal,copy->range)};
    TransferGuard guard{*state_};
    if (auto valid = prepare(uses); !valid) return valid;
    guard.emitted = true;
    if (auto valid = copy_to_image(*staging,destination,region); !valid) return valid;
    guard.committed = true;
    return {};
}
Result<ReadbackRequest> CommandBatch::readback(const Buffer& source, vk::DeviceSize offset, vk::DeviceSize size)
{
    if (auto valid = ready(state_); !valid) return std::unexpected(valid.error());
    if (auto valid = state_->check(source.state_); !valid) return std::unexpected(valid.error());
    if (offset > source.size()) return std::unexpected(Error{ErrorCode::invalid_argument,"readback offset exceeds buffer"});
    if (size == VK_WHOLE_SIZE) size = source.size()-offset;
    if (size > std::numeric_limits<std::size_t>::max()) return std::unexpected(Error{ErrorCode::invalid_argument,"readback exceeds host address space"});
    if (auto valid = detail::validate_copy(source.size(),size,size,offset,0,false); !valid) return std::unexpected(valid.error());
    if (!(source.state_->buffer_desc.usage & vk::BufferUsageFlagBits::eTransferSrc))
        return std::unexpected(Error{ErrorCode::invalid_argument,"readback source requires transfer usage"});
    auto staging = detail::ObjectAccess::factory(*state_).create_buffer({size,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback});
    if (!staging) return std::unexpected(staging.error());
    auto request = memory::make_shared_in<detail::ReadbackState>(state_->queue->resource,std::move(*staging),ReadbackDescription{size});
    TransferGuard guard{*state_};
    state_->requests.push_back(request); // All request ownership is allocated before recording/submission.
    const std::array uses{buffer_use(source,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,offset,size),
        buffer_use(request->staging,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite)};
    if (auto valid = prepare(uses); !valid) return std::unexpected(valid.error());
    guard.emitted = true;
    if (auto valid = copy_buffer(source,request->staging,size,offset); !valid) return std::unexpected(valid.error());
    const std::array host{buffer_use(request->staging,vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead)};
    if (auto valid = prepare(host); !valid) return std::unexpected(valid.error());
    guard.committed = true;
    return ReadbackRequest{std::move(request)};
}
Result<ReadbackRequest> CommandBatch::readback(const Image& source, const ImageCopyRegion& region)
{
    if (auto valid = ready(state_); !valid) return std::unexpected(valid.error());
    if (auto valid = state_->check(source.state_); !valid) return std::unexpected(valid.error());
    if (region.buffer_offset || region.row_length || region.image_height)
        return std::unexpected(Error{ErrorCode::invalid_argument,"image readback requests use tightly packed output at offset zero"});
    auto copy = detail::copy_footprint(source.description(),region);
    if (!copy) return std::unexpected(copy.error());
    if (copy->bytes > std::numeric_limits<std::size_t>::max() || !(source.description().usage & vk::ImageUsageFlagBits::eTransferSrc))
        return std::unexpected(Error{ErrorCode::invalid_argument,"readback size or image transfer usage invalid"});
    if (auto valid = check_image_read(*state_,source.state_,*copy); !valid) return std::unexpected(valid.error());
    auto staging = detail::ObjectAccess::factory(*state_).create_buffer({copy->bytes,vk::BufferUsageFlagBits::eTransferDst,BufferMemory::readback});
    if (!staging) return std::unexpected(staging.error());
    auto request = memory::make_shared_in<detail::ReadbackState>(state_->queue->resource,std::move(*staging),
        ReadbackDescription{copy->bytes,source.description().format,region.width,region.height,vk::DeviceSize{region.width}*4});
    TransferGuard guard{*state_};
    state_->requests.push_back(request);
    const std::array uses{image_use(source,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferRead,vk::ImageLayout::eTransferSrcOptimal,copy->range),
        buffer_use(request->staging,vk::PipelineStageFlagBits2::eCopy,vk::AccessFlagBits2::eTransferWrite)};
    if (auto valid = prepare(uses); !valid) return std::unexpected(valid.error());
    guard.emitted = true;
    if (auto valid = copy_to_buffer(source,request->staging,region); !valid) return std::unexpected(valid.error());
    const std::array host{buffer_use(request->staging,vk::PipelineStageFlagBits2::eHost,vk::AccessFlagBits2::eHostRead)};
    if (auto valid = prepare(host); !valid) return std::unexpected(valid.error());
    guard.committed = true;
    return ReadbackRequest{std::move(request)};
}
} // namespace dk::graphics
