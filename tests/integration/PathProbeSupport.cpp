#include "PathProbeSupport.hpp"

namespace dk::test::path_probe {
const PathTraceRay down{{0,0,1},{0,0,-1}};
bool fail_submit=false;

Fixture::Fixture(memory::ResourceHandle h):heap(h) {
    asset.mesh.id=AssetId::generate().value(); entity=document->create_entity().value();
    document->set_asset_references(entity,{{asset.mesh.id,AssetKind::mesh}}).value();
}
void Fixture::triangle(Vec3f a,Vec3f b,Vec3f c,Vec3f color,Vec3f emission,bool two) {
    MaterialData m; m.id=AssetId::generate().value(); m.metallic=0; m.base_color={color.x(),color.y(),color.z(),1}; m.emissive=emission; m.double_sided=two;
    asset.materials.push_back(m); MeshPrimitive p; p.positions={a,b,c}; p.indices={0,1,2}; p.material=m.id; asset.mesh.primitives.push_back(std::move(p));
}
void Fixture::quad(float lo,float hi,float z,Vec3f color,Vec3f emission,bool flip) {
    const Vec3f a{lo,lo,z},b{hi,lo,z},c{hi,hi,z},d{lo,hi,z};
    if (flip) { triangle(a,c,b,color,emission); triangle(a,d,c,color,emission); }
    else { triangle(a,b,c,color,emission); triangle(a,c,d,color,emission); }
}
RenderScene Fixture::source() const { return RenderScene::extract(heap,document->snapshot().value()).value(); }
PathTraceScene Fixture::scene() const { const std::array assets{&asset}; return take(PathTraceScene::create(heap,source(),assets)); }
RenderView Fixture::view(const RenderScene& source,std::uint32_t size) const {
    ViewDescription d{size,size}; Trsd camera; camera.translation={0,0,1}; d.camera_world=Transformd::from_trs(camera).value();
    d.projection.setZero(); d.projection(0,0)=2; d.projection(1,1)=-2;
    const double near=.1,far=10; d.projection(2,2)=far/(near-far); d.projection(2,3)=far*near/(near-far); d.projection(3,2)=-1;
    return take(RenderView::create(source,d));
}

void require(bool value,const char* message) { if (!value) throw std::runtime_error(message); }

void check(Result<void> r) { if (!r) throw std::runtime_error(r.error().message); }

PathTraceSettings settings(std::uint32_t depth) { PathTraceSettings s; s.max_bounces=depth; s.rr_start=0; return s; }

PathTraceGpuImage complete(SubmissionQueue& q,const PathTraceFrame& frame) {
    require(take(q.wait(frame.submission())),"GPU frame wait failed"); auto result=take(frame.read(q)); require(bool(result),"completed frame still pending"); return std::move(*result);
}

PathTraceGpuImage ray_image(PathTraceRenderer& renderer,SubmissionQueue& queue,const PathTraceGpuScene& scene,PathTraceSettings s,
    std::uint32_t samples,const PathTraceRay& ray,std::uint32_t first) {
    return complete(queue,take(renderer.submit_rays(queue,scene,std::span{&ray,1},s,{first,samples})));
}

void close_color(const Vec3d& value,const Vec3d& expected,double tolerance) {
    if ((value-expected).cwiseAbs().maxCoeff()>tolerance) { std::printf("actual %.9g %.9g %.9g expected %.9g %.9g %.9g\n",value.x(),value.y(),value.z(),expected.x(),expected.y(),expected.z()); throw std::runtime_error("linear color mismatch"); }
}

void add_texture(Fixture& f,std::size_t primitive,Vec2f uv) {
    TextureData texture; texture.id=AssetId::generate().value(); texture.width=texture.height=2;
    texture.origin="procedural/baseColor"; texture.sampler.mag_filter=TextureFilter::nearest;
    texture.rgba8={std::byte{128},std::byte{0},std::byte{0},std::byte{0},std::byte{0},std::byte{255},std::byte{0},std::byte{255},
        std::byte{0},std::byte{0},std::byte{255},std::byte{128},std::byte{255},std::byte{255},std::byte{255},std::byte{255}};
    f.asset.materials[primitive].base_color_texture=texture.id;
    f.asset.mesh.primitives[primitive].texcoords={uv,uv,uv}; f.asset.textures.push_back(std::move(texture));
}

PathTraceScene ggx_scene(const Fixture& f) {
    const std::array assets{&f.asset}; return take(PathTraceScene::create(f.heap,f.source(),assets,{},PathTraceMaterialModel::metallic_roughness));
}

bool collect(PathTraceRenderer& r,SubmissionQueue& q) {
    q.device().logical_device().waitIdle(); return take(r.poll(q));
}

void advance_view(PathTraceRenderer& r,SubmissionQueue& q,std::uint64_t id) {
    for(unsigned step=0;step<100000;++step) {
        const auto state=take(r.status(id));
        if(state.completed_spp>=state.target_spp && !state.pending)
            return;
        take(r.submit_batch(q,id)); collect(r,q);
    }
    throw std::runtime_error("progressive view failed to finish");
}

PathTraceGpuImage finish_view(PathTraceRenderer& r,SubmissionQueue& q,std::uint64_t id) {
    advance_view(r,q,id); return complete(q,take(r.completed_frame(id).read_linear(q)));
}

void compare_images(const PathTraceGpuImage& a,const PathTraceGpuImage& b,double tolerance) {
    require(a.linear_rgb().size()==b.linear_rgb().size(),"progressive image size mismatch");
    for(std::size_t i=0;i<a.linear_rgb().size();++i) {
        close_color(a.linear_rgb()[i],b.linear_rgb()[i],tolerance);
        close_color(a.second_moment()[i],b.second_moment()[i],tolerance);
        require(bool(a.primary_hits()[i])==bool(b.primary_hits()[i]),"progressive primary hit changed");
        if(a.primary_hits()[i]) require(a.primary_hits()[i]->index==b.primary_hits()[i]->index,"first sample hit changed");
    }
    require(a.info().statistics.rays==b.info().statistics.rays,"progressive ray accounting changed");
}

Vec3d signal_rgb(const DenoisingSignalPixel::Float4& v) { return {v[0],v[1],v[2]}; }

void signal_recompose(const PathTraceGpuImage& image) {
    require(image.info().signals.has_value(),"signal provenance missing");
    const auto& source=image.info().signals->source;
    require(source.version==2 && source.content_version && source.source_serial && source.renderer_id && source.width==image.info().width &&
        source.height==image.info().height && source.fingerprint==image.info().fingerprint &&
        source.sample_first+source.sample_count==source.accumulated_spp,"signal source identity mismatch");
    require(image.denoising_signals().size()==image.linear_rgb().size(),"partial signal image published");
    for(const auto& p:image.denoising_signals()) {
        const Vec3d raw=signal_rgb(p.fresh_sum),sum=signal_rgb(p.diffuse_sum)+signal_rgb(p.specular_sum)+signal_rgb(p.emission_sum)+signal_rgb(p.background_sum);
        close_color(sum,raw,2e-5*std::max(1.,raw.maxCoeff()));
        require(p.guide_samples()==source.accumulated_spp && p.identity[3]==source.sample_count && p.identity[1]==source.sampling_first_epoch,"guide/fresh coverage mismatch");
    }
}
} // namespace dk::test::path_probe
