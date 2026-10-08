#include <dk/geometry/Query.hpp>
#include <algorithm>
#include <cmath>
#include <numeric>

namespace dk::geometry {
namespace {
std::optional<double> box(const Ray& ray,const Bounds& b,double limit) {
    if (b.empty()) return {};
    double lo=ray.minimum,hi=std::min(ray.maximum,limit);
    for (int i=0;i<3;++i) {
        if (ray.direction[i]==0) { if (ray.origin[i]<b.minimum[i] || ray.origin[i]>b.maximum[i]) return {}; }
        else {
            double a=(b.minimum[i]-ray.origin[i])/ray.direction[i],c=(b.maximum[i]-ray.origin[i])/ray.direction[i];
            if (a>c) std::swap(a,c);
            lo=std::max(lo,a); hi=std::min(hi,c);
            if (lo>hi) return {};
        }
    }
    return lo;
}
std::optional<Hit> triangle(const Ray& r,const Triangle& t,std::size_t index) {
    const Vec3d e1=t.b-t.a,e2=t.c-t.a,p=r.direction.cross(e2),s=r.origin-t.a;
    const double determinant=e1.dot(p);
    const double tolerance=1e-14*e1.norm()*p.norm();
    if (!std::isfinite(determinant) || std::abs(determinant)<=tolerance) return {};
    const double u=s.dot(p)/determinant;
    const Vec3d q=s.cross(e1);
    const double v=r.direction.dot(q)/determinant,d=e2.dot(q)/determinant;
    if (!std::isfinite(d) || u<0 || v<0 || u+v>1 || d<r.minimum || d>r.maximum) return {};
    return Hit{d,index,{1-u-v,u,v}};
}
}
Result<void> validate(const Ray& r) {
    if (!r.origin.allFinite() || !r.direction.allFinite() || r.direction.cwiseAbs().maxCoeff()==0 ||
        !std::isfinite(r.minimum) || !std::isfinite(r.maximum) || r.minimum<0 || r.maximum<r.minimum)
        return std::unexpected(Error{ErrorCode::invalid_argument,"Ray requires finite origin/direction and nonnegative ordered t interval"});
    return {};
}
Result<std::optional<double>> intersect(const Ray& r,const Bounds& b) {
    if (auto valid=validate(r); !valid) return std::unexpected(valid.error());
    if (!b.minimum.allFinite() || !b.maximum.allFinite()) return std::unexpected(Error{ErrorCode::invalid_argument,"Nonfinite bounds"});
    return box(r,b,r.maximum);
}
Result<std::optional<Hit>> intersect(const Ray& r,const Triangle& t) {
    if (auto valid=validate(r); !valid) return std::unexpected(valid.error());
    if (!t.a.allFinite() || !t.b.allFinite() || !t.c.allFinite()) return std::unexpected(Error{ErrorCode::invalid_argument,"Nonfinite triangle"});
    return triangle(r,t,0);
}
Result<MeshQuery> MeshQuery::build(std::span<const Triangle> input) {
    for (const auto& t:input) if (!t.a.allFinite() || !t.b.allFinite() || !t.c.allFinite())
        return std::unexpected(Error{ErrorCode::invalid_argument,"Nonfinite mesh position"});
    try {
        MeshQuery result;
        result.triangles_.assign(input.begin(),input.end()); result.order_.resize(input.size());
        std::iota(result.order_.begin(),result.order_.end(),std::size_t{0});
        if (!input.empty()) { result.split(0,input.size()); result.bounds_=result.nodes_.front().bounds; }
        return result;
    } catch (const std::bad_alloc&) { return std::unexpected(Error{ErrorCode::internal_error,"Mesh query allocation failed"}); }
}
std::size_t MeshQuery::split(std::size_t first,std::size_t count) {
    const auto index=nodes_.size(); nodes_.emplace_back();
    Bounds bounds,centers;
    const auto center=[&](std::size_t i) -> Vec3d { const auto& t=triangles_[i]; return t.a/3+t.b/3+t.c/3; };
    for (std::size_t i=first;i<first+count;++i) {
        const auto& t=triangles_[order_[i]];
        bounds.include(t.a); bounds.include(t.b); bounds.include(t.c); centers.include(center(order_[i]));
    }
    nodes_[index]={bounds,first,count};
    if (count<=8) return index;
    Eigen::Index axis=0; (centers.maximum-centers.minimum).maxCoeff(&axis);
    const auto middle=first+count/2;
    std::nth_element(order_.begin()+static_cast<std::ptrdiff_t>(first),order_.begin()+static_cast<std::ptrdiff_t>(middle),
        order_.begin()+static_cast<std::ptrdiff_t>(first+count),[&](auto a,auto b) { return center(a)[axis]<center(b)[axis]; });
    const auto left=split(first,count/2),right=split(middle,count-count/2);
    nodes_[index].count=0; nodes_[index].left=left; nodes_[index].right=right;
    return index;
}
void MeshQuery::visit(std::size_t index,const Ray& ray,std::optional<Hit>& best) const {
    const auto& node=nodes_[index];
    if (!box(ray,node.bounds,best ? best->distance : ray.maximum)) return;
    if (!node.count) { visit(node.left,ray,best); visit(node.right,ray,best); return; }
    for (std::size_t i=node.first;i<node.first+node.count;++i) {
        auto hit=triangle(ray,triangles_[order_[i]],order_[i]);
        if (hit && (!best || hit->distance<best->distance || (hit->distance==best->distance && hit->triangle<best->triangle))) best=hit;
    }
}
Result<std::optional<Hit>> MeshQuery::nearest(const Ray& r) const {
    if (auto valid=validate(r); !valid) return std::unexpected(valid.error());
    std::optional<Hit> result;
    if (!nodes_.empty()) visit(0,r,result);
    return result;
}
Result<std::optional<Hit>> MeshQuery::nearest(const Ray& r,const Transformd& world) const {
    if (auto valid=validate(r); !valid) return std::unexpected(valid.error());
    auto inverse=world.inverse(); if (!inverse) return std::unexpected(inverse.error());
    auto origin=inverse->transform_point(r.origin),direction=inverse->transform_direction(r.direction);
    if (!origin) return std::unexpected(origin.error());
    if (!direction) return std::unexpected(direction.error());
    return nearest({*origin,*direction,r.minimum,r.maximum});
}
}
