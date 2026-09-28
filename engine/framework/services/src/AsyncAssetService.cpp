#include <dk/services/AsyncAssetService.hpp>
#include <dk/memory/MemorySystem.hpp>

namespace dk {
namespace {
memory::MemorySystem system() { auto r = memory::MemorySystem::create(); if (!r) throw std::bad_alloc{}; return std::move(*r); }
memory::ResourceHandle heap(memory::MemorySystem& s, memory::HeapOptions options) {
    auto r = s.create_heap(options); if (!r) throw std::bad_alloc{}; return *r;
}
}
struct AsyncAssetService::Impl {
    memory::MemorySystem memory = system();
    memory::ResourceHandle assets_heap = heap(memory, {"runtime-assets", memory::DomainCategory::assets});
    memory::ResourceHandle jobs_heap = heap(memory, {"runtime-jobs", memory::DomainCategory::jobs});
    memory::ThreadContext context{memory, assets_heap};
    ProjectPaths paths;
    JobQueue queue;
    std::unique_ptr<AsyncAssets> assets;
    std::unique_ptr<AssetService> catalog;
    std::string manifest;
    Impl(ProjectPaths p, std::function<void()> wake) : paths(std::move(p)), queue(jobs_heap, {}, std::move(wake)) {
        memory::ExecutionScope scope(context, assets_heap); assets = std::make_unique<AsyncAssets>(paths, queue);
    }
    Result<std::string_view> source(AssetId id) const {
        if (!catalog) return std::unexpected(Error{ErrorCode::invalid_state, "Open an asset catalog first"});
        for (const auto& r : catalog->catalog().records()) if (r.id == id) return std::string_view{r.path};
        return std::unexpected(Error{ErrorCode::not_found, "AssetId is not registered in the active catalog"});
    }
};
AsyncAssetService::AsyncAssetService(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
Result<std::unique_ptr<AsyncAssetService>> AsyncAssetService::create(const std::filesystem::path& root, std::function<void()> wake) {
    auto paths = ProjectPaths::create(root); if (!paths) return std::unexpected(paths.error());
    return std::unique_ptr<AsyncAssetService>(new AsyncAssetService(std::make_unique<Impl>(std::move(*paths), std::move(wake))));
}
AsyncAssetService::~AsyncAssetService() { close(); }
void AsyncAssetService::close() noexcept { impl_->queue.close(); }
Result<void> AsyncAssetService::open(std::string_view manifest, std::optional<CatalogGuard> guard) {
    auto& s = *impl_; memory::ExecutionScope scope(s.context, s.assets_heap);
    if (s.catalog) {
        if (!guard) return std::unexpected(Error{ErrorCode::conflict, "Replacing the catalog requires its guard"});
        auto check = s.catalog->catalog().check_guard(*guard); if (!check) return check;
    } else if (guard) return std::unexpected(Error{ErrorCode::conflict, "No catalog matches this guard"});
    auto opened = AssetService::open(s.paths.root(), manifest); if (!opened) return std::unexpected(opened.error());
    auto next = std::make_unique<AssetService>(std::move(*opened)); std::string next_name{manifest};
    auto reset = s.assets->reset_session(); if (!reset) return reset;
    s.catalog = std::move(next); s.manifest = std::move(next_name); return {};
}
Result<const AssetService*> AsyncAssetService::catalog() const {
    if (!impl_->catalog) return std::unexpected(Error{ErrorCode::invalid_state, "No active asset catalog"});
    return impl_->catalog.get();
}
Result<void> AsyncAssetService::register_source(CatalogGuard guard, const RegistrationRequest& request) {
    auto& s = *impl_; memory::ExecutionScope scope(s.context, s.assets_heap);
    auto active = catalog(); if (!active) return std::unexpected(active.error());
    const auto previous = s.catalog->catalog().guard();
    auto result = s.catalog->register_source(guard, request);
    if (result && s.catalog->catalog().guard() != previous) return s.assets->reset_session(); return result;
}
Result<void> AsyncAssetService::rename_source(CatalogGuard guard, std::string_view source, std::string_view target) {
    auto& s = *impl_; memory::ExecutionScope scope(s.context, s.assets_heap);
    auto active = catalog(); if (!active) return std::unexpected(active.error());
    auto result = s.catalog->rename_source(guard, source, target);
    if (result) return s.assets->reset_session(); return result;
}
Result<JobId> AsyncAssetService::import(std::string_view source, std::optional<double> scale) {
    auto& s = *impl_; memory::ExecutionScope scope(s.context, s.assets_heap);
    if (s.catalog) for (const auto& record : s.catalog->catalog().records()) if (std::string_view{record.path} == source) {
        auto meta = s.catalog->catalog().inspect_source(source);
        if (!meta) return std::unexpected(meta.error().with_context("Register legacy identities with create_meta before importing"));
        break;
    }
    return s.assets->import(source,scale);
}
Result<AssetStatus> AsyncAssetService::load(AssetId id) {
    auto& s = *impl_; memory::ExecutionScope scope(s.context, s.assets_heap);
    auto source = s.source(id); if (!source) return std::unexpected(source.error()); return s.assets->load(id,*source);
}
Result<AssetStatus> AsyncAssetService::status(AssetId id) {
    auto& s = *impl_; memory::ExecutionScope scope(s.context, s.assets_heap);
    auto source = s.source(id); if (!source) return std::unexpected(source.error()); return s.assets->status(id,*source);
}
Result<AssetStatus> AsyncAssetService::unload(AssetId id) {
    auto& s = *impl_; memory::ExecutionScope scope(s.context, s.assets_heap);
    auto source = s.source(id); if (!source) return std::unexpected(source.error()); return s.assets->unload(id,*source);
}
Result<JobSnapshot> AsyncAssetService::job(JobId id) const { return impl_->queue.query(id); }
Result<JobWait> AsyncAssetService::wait(JobId id, std::chrono::milliseconds timeout) {
    if (timeout.count() < 0 || timeout.count() > 1000) return std::unexpected(Error{ErrorCode::invalid_argument, "timeout_ms must be 0..1000"});
    const auto deadline = JobQueue::Clock::now() + timeout;
    for (;;) {
        const auto sequence = impl_->queue.change_sequence(); pump();
        auto state = job(id); if (!state) return std::unexpected(state.error());
        if (terminal(state->state)) return JobWait{*state,false};
        if (JobQueue::Clock::now() >= deadline) return JobWait{*state,true};
        impl_->queue.wait_change(sequence,deadline);
    }
}
Result<JobCancel> AsyncAssetService::cancel(JobId id) { return impl_->queue.request_cancel(id); }
JobLimits AsyncAssetService::limits() const noexcept { return impl_->queue.limits(); }
void AsyncAssetService::pump() {
    auto& s = *impl_; memory::ExecutionScope scope(s.context, s.assets_heap); s.assets->pump();
}
void AsyncAssetService::rethrow_failure() const { impl_->queue.rethrow_failure(); }
Result<void> AsyncAssetService::refresh_manifest(std::string_view manifest) {
    auto& s = *impl_; memory::ExecutionScope scope(s.context,s.assets_heap);
    if (!s.catalog) return {};
    auto a = path_from_utf8(manifest), b = path_from_utf8(s.manifest);
    if (!a) return std::unexpected(a.error()); if (!b) return std::unexpected(b.error());
    auto pa = s.paths.resolve(*a), pb = s.paths.resolve(*b);
    if (!pa) return std::unexpected(pa.error()); if (!pb) return std::unexpected(pb.error());
    std::error_code error;
    if (std::filesystem::equivalent(*pa,*pb,error) && !error) return s.catalog->refresh_manifest();
    return {};
}
Result<void> AsyncAssetService::synchronize_scene(SceneService& scene) const {
    if (!impl_->catalog) return {};
    auto name = path_from_utf8(impl_->manifest); if (!name) return std::unexpected(name.error());
    return scene.synchronize_assets(impl_->catalog->project(), *name);
}
}
