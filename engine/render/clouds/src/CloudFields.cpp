#include <dk/render/CloudFields.hpp>
#include <algorithm>
#include <cmath>
#include <limits>

namespace dk::render {
namespace {
// Generator v1: specified unsigned wraparound; no standard RNG or floating point.
std::uint32_t hash(std::uint32_t v) {
    v ^= v >> 16; v *= 0x7feb352du; v ^= v >> 15; v *= 0x846ca68bu; return v ^ (v >> 16);
}
std::uint32_t smooth(std::uint32_t t) {
    return static_cast<std::uint32_t>((std::uint64_t(t) * t * (196608u - 2 * t)) >> 32);
}
std::uint32_t lerp(std::uint32_t a, std::uint32_t b, std::uint32_t t) {
    return static_cast<std::uint32_t>((std::uint64_t(a) * (65536u - t) + std::uint64_t(b) * t) >> 16);
}
std::uint32_t noise(std::uint32_t seed, unsigned x, unsigned y, unsigned z, unsigned n, unsigned cells, bool flat) {
    const auto axis = [=](unsigned p) { const auto q = std::uint64_t(p) * cells * 65536 / n;
        return std::array<unsigned, 2>{static_cast<unsigned>(q >> 16), smooth(static_cast<unsigned>(q & 65535))}; };
    const auto u=axis(x), v=axis(y), w=axis(z);
    const auto value = [=](unsigned i, unsigned j, unsigned k) {
        return hash(seed ^ hash((i % cells) + 0x9e3779b9u) ^ hash((j % cells) + 0x85ebca6bu) ^
            hash((k % cells) + 0xc2b2ae35u)) & 65535u;
    };
    const auto plane = [&](unsigned k) { return lerp(lerp(value(u[0],v[0],k), value(u[0]+1,v[0],k),u[1]),
        lerp(value(u[0],v[0]+1,k), value(u[0]+1,v[0]+1,k),u[1]),v[1]); };
    return flat ? plane(0) : lerp(plane(w[0]),plane(w[0]+1),w[1]);
}
std::vector<std::uint8_t> generate(unsigned n, bool flat, unsigned channels, std::uint32_t seed, unsigned first_cells) {
    std::vector<std::uint8_t> bytes(std::size_t(n)*n*(flat ? 1 : n)*channels);
    for(unsigned z=0;z<(flat ? 1u : n);++z) for(unsigned y=0;y<n;++y) for(unsigned x=0;x<n;++x)
        for(unsigned c=0;c<channels;++c) {
            unsigned sum=0, weight=8, weights=0;
            for(unsigned octave=0;octave<4;++octave,weight/=2) {
                sum += weight*noise(hash(seed+c*101u+octave*7919u),x,y,z,n,std::min(n,first_cells<<octave),flat);
                weights += weight;
            }
            bytes[((std::size_t(z)*n+y)*n+x)*channels+c]=static_cast<std::uint8_t>((sum/weights+128)/257);
        }
    return bytes;
}
double smooth01(double x) { x=std::clamp(x,0.0,1.0); return x*x*(3-2*x); }
Error invalid(const char* message) { return {ErrorCode::invalid_argument,message}; }
}
CloudFieldKey cloud_field_key(const CloudParameters& c) noexcept {
    return {c.seed,c.generator_version,c.weather_resolution,c.base_resolution,c.detail_resolution};
}
std::size_t CloudFieldData::bytes() const noexcept { return weather_.size()+base_.size()+detail_.size(); }
std::span<const std::uint8_t> CloudFieldData::texels(CloudField field) const noexcept {
    switch(field) { case CloudField::weather: return weather_; case CloudField::base: return base_; case CloudField::detail: return detail_; }
    return {};
}
Result<std::array<double,2>> CloudFieldData::sample(CloudField field, EnvironmentVector uvw) const {
    const auto data=texels(field);
    if(data.empty()) return std::unexpected(invalid("Empty or unknown cloud field"));
    const bool flat=field==CloudField::weather;
    const unsigned n=flat ? key_.weather_resolution : field==CloudField::base ? key_.base_resolution : key_.detail_resolution;
    const unsigned channels=flat ? 2u : 1u;
    std::array<unsigned,3> lo{}; EnvironmentVector t{};
    for(unsigned a=0;a<3;++a) {
        if(!std::isfinite(uvw[a])) return std::unexpected(invalid("Invalid cloud sample coordinate"));
        const double p=(uvw[a]-std::floor(uvw[a]))*n-.5;
        const auto floor=std::floor(p); t[a]=p-floor;
        lo[a]=static_cast<unsigned>(static_cast<int>(floor)+static_cast<int>(n))%n;
    }
    std::array<double,2> result{};
    for(unsigned c=0;c<channels;++c) for(unsigned z=0;z<(flat ? 1u : 2u);++z) for(unsigned y=0;y<2;++y) for(unsigned x=0;x<2;++x) {
        const auto index=((std::size_t(flat ? 0 : (lo[2]+z)%n)*n+(lo[1]+y)%n)*n+(lo[0]+x)%n)*channels+c;
        result[c] += data[index]/255.0*(x ? t[0] : 1-t[0])*(y ? t[1] : 1-t[1])*(flat ? 1 : z ? t[2] : 1-t[2]);
    }
    return result;
}
Result<std::shared_ptr<const CloudFieldData>> CloudFieldCache::prepare(const CloudParameters& c) {
    if(auto valid=validate_clouds(c,1e7); !valid) return std::unexpected(valid.error());
    if(!limits_.generations || limits_.generations>2 || !limits_.bytes || limits_.bytes>16*1024*1024)
        return std::unexpected(invalid("Invalid cloud field limits"));
    const auto key=cloud_field_key(c);
    if(current_ && current_->key()==key) return current_;
    const auto cube=[](std::size_t n) { return n*n*n; };
    const std::size_t bytes=std::size_t(c.weather_resolution)*c.weather_resolution*2+cube(c.base_resolution)+cube(c.detail_resolution);
    std::size_t occupied=bytes, count=1;
    for(const auto& weak:live_) if(const auto live=weak.lock()) { occupied+=live->bytes(); ++count; }
    if(occupied>limits_.bytes || count>limits_.generations)
        return std::unexpected(Error{ErrorCode::invalid_state,"Cloud field generation or byte budget exceeded"});
    if(current_ && current_->generation()==std::numeric_limits<std::uint64_t>::max())
        return std::unexpected(Error{ErrorCode::invalid_state,"Cloud field generation exhausted"});
    auto candidate=std::make_shared<CloudFieldData>(); candidate->key_=key;
    candidate->generation_=current_ ? current_->generation()+1 : 1;
    candidate->weather_=generate(c.weather_resolution,true,2,c.seed,4);
    candidate->base_=generate(c.base_resolution,false,1,c.seed^0x6a09e667u,4);
    candidate->detail_=generate(c.detail_resolution,false,1,c.seed^0xbb67ae85u,2);
    candidate->hash_=14695981039346656037ULL;
    for(const auto field:{CloudField::weather,CloudField::base,CloudField::detail})
        for(auto v:candidate->texels(field)) { candidate->hash_^=v; candidate->hash_*=1099511628211ULL; }
    std::erase_if(live_,[](const auto& weak){ return weak.expired(); });
    live_.push_back(candidate); // Allocation before the no-throw publication point.
    current_=std::move(candidate); return current_;
}
Result<double> sample_cloud_density(const EnvironmentDescription& e,const CloudFieldData& fields,const EnvironmentVector& position,double time) {
    if(auto valid=normalize_environment(e); !valid) return std::unexpected(valid.error());
    if(!std::isfinite(time) || std::abs(time)>1e9) return std::unexpected(invalid("Invalid cloud time"));
    const auto height=atmosphere_height_m(e.atmosphere,position);
    if(!height) return std::unexpected(height.error());
    const auto& c=e.clouds;
    if(fields.key()!=cloud_field_key(c)) return std::unexpected(invalid("Cloud field key does not match parameters"));
    if(!c.enabled || !c.coverage || !c.density || *height<=c.bottom_height_m || *height>=c.top_height_m) return 0.0;
    EnvironmentVector p{position[0]-e.atmosphere.planet_center_m[0]-c.wind_velocity_mps[0]*time,
        position[1]-e.atmosphere.planet_center_m[1],position[2]-e.atmosphere.planet_center_m[2]-c.wind_velocity_mps[1]*time};
    const auto weather=fields.sample(CloudField::weather,{p[0]/c.weather_scale_m,p[2]/c.weather_scale_m,0});
    auto scale=[&](double s){ return EnvironmentVector{p[0]/s,p[1]/s,p[2]/s}; };
    const auto base=fields.sample(CloudField::base,scale(c.shape_scale_m));
    const auto detail=fields.sample(CloudField::detail,scale(c.detail_scale_m));
    if(!weather) return std::unexpected(weather.error());
    if(!base) return std::unexpected(base.error());
    if(!detail) return std::unexpected(detail.error());
    const double coverage=std::clamp(((*weather)[0]-(1-c.coverage))/c.coverage,0.0,1.0);
    const double h=(*height-c.bottom_height_m)/(c.top_height_m-c.bottom_height_m);
    const double top=.6+.4*(*weather)[1];
    const double profile=smooth01(h/c.bottom_fade_fraction)*smooth01((top-h)/c.top_fade_fraction);
    return c.density*std::clamp(((*base)[0]*profile-(1-coverage))/.35-.2*(*detail)[0],0.0,1.0);
}
} // namespace dk::render
