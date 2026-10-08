#include <dk/editor/TransformEdit.hpp>
#include <algorithm>
#include <cmath>

namespace dk::editor {
Result<Transformd> entity_world(const SceneSnapshot& scene,EntityId id) {
    Transformd result;
    for (std::size_t depth=0;depth<=scene.entities().size();++depth) {
        const auto e=std::ranges::find(scene.entities(),id,&EntityData::id);
        if (e==scene.entities().end()) return std::unexpected(Error{ErrorCode::not_found,"Transform entity/parent missing"});
        auto local=Transformd::from_trs(e->local); if (!local) return std::unexpected(local.error());
        auto composed=local->compose(result); if (!composed) return std::unexpected(composed.error());
        result=*composed;
        if (!e->parent) return result;
        id=*e->parent;
    }
    return std::unexpected(Error{ErrorCode::invalid_state,"Cyclic transform hierarchy"});
}
Result<void> TransformEdit::translate(const Vec3d& delta) {
    auto inverse=parent.inverse(); if (!inverse) return std::unexpected(inverse.error());
    auto local=inverse->transform_direction(delta); if (!local) return std::unexpected(local.error());
    auto candidate=original; candidate.translation+=*local;
    auto valid=Transformd::from_trs(candidate); if (!valid) return std::unexpected(valid.error());
    value=candidate; return {};
}
Result<void> TransformEdit::rotate(unsigned axis,double radians) {
    if (axis>2 || !std::isfinite(radians)) return std::unexpected(Error{ErrorCode::invalid_argument,"Invalid local rotation"});
    auto candidate=original;
    candidate.rotation=(original.rotation.normalized()*Quatd{Eigen::AngleAxisd{radians,Vec3d::Unit(static_cast<int>(axis))}}).normalized();
    auto valid=Transformd::from_trs(candidate); if (!valid) return std::unexpected(valid.error());
    value=candidate; return {};
}
Result<void> TransformEdit::scale(unsigned axis,double factor) {
    if (axis>2 || !std::isfinite(factor) || factor<=0) return std::unexpected(Error{ErrorCode::invalid_argument,"Invalid local scale factor"});
    auto candidate=original; candidate.scale[axis]*=factor;
    auto valid=Transformd::from_trs(candidate); if (!valid) return std::unexpected(valid.error());
    value=candidate; return {};
}
bool TransformEdit::changed() const {
    return !value.translation.isApprox(original.translation,1e-12) || !value.scale.isApprox(original.scale,1e-12) ||
        std::abs(value.rotation.normalized().dot(original.rotation.normalized()))<1-1e-12;
}
Result<Transformd> TransformEdit::world() const {
    auto local=Transformd::from_trs(value); if (!local) return std::unexpected(local.error());
    return parent.compose(*local);
}
}
