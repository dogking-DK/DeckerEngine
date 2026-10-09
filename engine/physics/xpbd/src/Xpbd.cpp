#include <dk/physics/Xpbd.hpp>
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace dk {
namespace {
std::unexpected<Error> invalid(std::string message) { return std::unexpected(Error{ErrorCode::invalid_argument, std::move(message)}); }
std::unexpected<Error> numerical() { return std::unexpected(Error{ErrorCode::invalid_state, "XPBD numerical state exceeded finite coordinate/velocity bounds"}); }
bool bounded(float x, float low, float high) { return std::isfinite(x) && x >= low && x <= high; }
bool config_valid(XpbdConfig c) {
    return c.iterations >= 1 && c.iterations <= 32 && bounded(c.gravity_y, -1000, 1000)
        && bounded(c.floor_y, -10000, 10000) && bounded(c.damping, 0, 100);
}
template<class T> bool finite_vector(const T& p) {
    return bounded(p.x, -1e6f, 1e6f) && bounded(p.y, -1e6f, 1e6f) && bounded(p.z, -1e6f, 1e6f);
}
float distance(const ParticlePosition& a, const ParticlePosition& b) {
    const float x = a.x - b.x, y = a.y - b.y, z = a.z - b.z;
    return std::sqrt(x*x + y*y + z*z);
}
}
Result<XpbdSolver> XpbdSolver::create(XpbdConfig config, std::vector<ParticlePosition> positions,
    std::vector<ParticleVelocity> velocities, std::vector<DistanceConstraint> constraints) {
    if (!config_valid(config)) return invalid("Invalid XPBD configuration");
    if (positions.empty() || positions.size() > 1024 || constraints.size() > 8192 || velocities.size() != positions.size())
        return invalid("XPBD requires 1-1024 particles, matching velocities and at most 8192 constraints");
    for (std::size_t i = 0; i < positions.size(); ++i) {
        const auto& p = positions[i]; const auto& v = velocities[i];
        if (!finite_vector(p) || !finite_vector(v) || !bounded(p.inverse_mass, 0, 1e6f) || v.padding != 0)
            return invalid("Invalid XPBD particle data");
        if (p.inverse_mass == 0 && (p.y < config.floor_y || v.x != 0 || v.y != 0 || v.z != 0))
            return invalid("Fixed particles must be above the floor with zero velocity");
    }
    std::unordered_set<std::uint64_t> edges;
    std::vector<std::uint32_t> masks(positions.size(), 0), colors;
    std::uint32_t color_count = 0;
    for (const auto& c : constraints) {
        if (c.a >= positions.size() || c.b >= positions.size() || c.a == c.b
            || !bounded(c.rest_length, 1e-6f, 1e6f) || !bounded(c.compliance, 0, 1e6f))
            return invalid("Invalid XPBD distance constraint");
        const auto key = (static_cast<std::uint64_t>(std::min(c.a, c.b)) << 32) | std::max(c.a, c.b);
        if (!edges.insert(key).second || distance(positions[c.a], positions[c.b]) < 1e-7f)
            return invalid("Duplicate or initially coincident XPBD constraint endpoints");
        const auto color = static_cast<std::uint32_t>(std::countr_one(masks[c.a] | masks[c.b]));
        if (color == 32) return invalid("XPBD constraint graph exceeds 32 colors");
        masks[c.a] |= std::uint32_t{1} << color; masks[c.b] |= std::uint32_t{1} << color;
        colors.push_back(color); color_count = std::max(color_count, color + 1);
    }
    XpbdSolver solver;
    solver.config_ = config; solver.positions_ = std::move(positions); solver.initial_ = solver.positions_;
    solver.velocities_ = std::move(velocities);
    for (std::uint32_t color = 0; color < color_count; ++color) {
        solver.color_offsets_.push_back(static_cast<std::uint32_t>(solver.constraints_.size()));
        for (std::size_t i = 0; i < constraints.size(); ++i) if (colors[i] == color) {
            const auto& c = constraints[i]; const auto& a = solver.positions_[c.a]; const auto& b = solver.positions_[c.b];
            const auto length = distance(a, b);
            solver.constraints_.push_back(c);
            solver.directions_.push_back({(a.x-b.x)/length, (a.y-b.y)/length, (a.z-b.z)/length});
        }
    }
    solver.color_offsets_.push_back(static_cast<std::uint32_t>(solver.constraints_.size()));
    solver.metrics_ = solver.measure(solver.positions_, solver.velocities_);
    return solver;
}
Result<XpbdSolver> XpbdSolver::cloth(ClothConfig c) {
    if (c.columns < 2 || c.columns > 32 || c.rows < 2 || c.rows > 32
        || !bounded(c.spacing, 0.01f, 1) || !bounded(c.height, -100, 100) || c.height <= c.physics.floor_y
        || !bounded(c.particle_mass, 0.001f, 100) || !bounded(c.compliance, 0, 0.01f) || !config_valid(c.physics))
        return invalid("Invalid XPBD cloth configuration");
    std::vector<ParticlePosition> p;
    auto random = c.seed;
    for (std::uint32_t row = 0; row < c.rows; ++row) for (std::uint32_t col = 0; col < c.columns; ++col) {
        float y = c.height;
        if (row != 0) {
            random = 1664525u * random + 1013904223u;
            y += 0.01f * c.spacing * (static_cast<float>(random >> 8) / 16777216.0f);
        }
        p.push_back({(static_cast<float>(col) - 0.5f*static_cast<float>(c.columns-1))*c.spacing,
            y, static_cast<float>(row)*c.spacing, row == 0 ? 0.0f : 1.0f/c.particle_mass});
    }
    std::vector<DistanceConstraint> constraints;
    const auto add = [&](std::uint32_t a, std::uint32_t b) { constraints.push_back({a, b, distance(p[a], p[b]), c.compliance}); };
    for (std::uint32_t row = 0; row < c.rows; ++row) for (std::uint32_t col = 0; col < c.columns; ++col) {
        const auto i = row*c.columns + col;
        if (col+1 < c.columns) add(i, i+1);
        if (row+1 < c.rows) add(i, i+c.columns);
        if (col+1 < c.columns && row+1 < c.rows) { add(i, i+c.columns+1); add(i+1, i+c.columns); }
    }
    std::vector<ParticleVelocity> v(p.size());
    return create(c.physics, std::move(p), std::move(v), std::move(constraints));
}
Result<void> XpbdSolver::advance(std::int64_t fixed_dt_ns, std::uint32_t count) {
    if (fixed_dt_ns < 1000000 || fixed_dt_ns > 33333333 || count < 1 || count > 10000)
        return invalid("XPBD requires dt=1000000..33333333 ns and count=1..10000");
    const auto work = static_cast<std::uint64_t>(count) * (positions_.size()
        + config_.iterations * (positions_.size() + constraints_.size()));
    if (work > 20000000) return invalid("XPBD synchronous work budget exceeded; split the requested steps");
    const auto dt = static_cast<float>(fixed_dt_ns) * 1e-9f;
    auto p = positions_; auto v = velocities_; auto previous = p;
    std::vector<float> lambda(constraints_.size());
    for (std::uint32_t step = 0; step < count; ++step) {
        std::copy(p.begin(), p.end(), previous.begin());
        std::fill(lambda.begin(), lambda.end(), 0.0f);
        const auto damping = 1.0f / (1.0f + config_.damping*dt);
        for (std::size_t i = 0; i < p.size(); ++i) if (p[i].inverse_mass != 0) {
            v[i].x *= damping; v[i].y = (v[i].y + dt*config_.gravity_y)*damping; v[i].z *= damping;
            p[i].x += dt*v[i].x; p[i].y += dt*v[i].y; p[i].z += dt*v[i].z;
            if (!finite_vector(p[i])) return numerical();
        }
        for (std::uint32_t iteration = 0; iteration < config_.iterations; ++iteration) {
            // Contiguous constraints are ordered by color; each color has disjoint endpoints.
            for (std::size_t j = 0; j < constraints_.size(); ++j) {
                const auto& c = constraints_[j]; auto& a = p[c.a]; auto& b = p[c.b];
                const auto weight = a.inverse_mass + b.inverse_mass; if (weight == 0) continue;
                const auto length = distance(a, b);
                const auto n = length < 1e-7f ? directions_[j] : std::array<float,3>{(a.x-b.x)/length, (a.y-b.y)/length, (a.z-b.z)/length};
                const auto alpha = c.compliance / (dt*dt);
                const auto delta = (-(length - c.rest_length) - alpha*lambda[j]) / (weight + alpha);
                lambda[j] += delta;
                const auto wa = a.inverse_mass*delta, wb = b.inverse_mass*delta;
                a.x += wa*n[0]; a.y += wa*n[1]; a.z += wa*n[2];
                b.x -= wb*n[0]; b.y -= wb*n[1]; b.z -= wb*n[2];
            }
            for (auto& particle : p) if (particle.inverse_mass != 0) particle.y = std::max(particle.y, config_.floor_y);
        }
        for (std::size_t i = 0; i < p.size(); ++i) {
            if (p[i].inverse_mass != 0) {
                v[i] = {(p[i].x-previous[i].x)/dt, (p[i].y-previous[i].y)/dt, (p[i].z-previous[i].z)/dt, 0};
                if (p[i].y <= config_.floor_y && v[i].y < 0) v[i].y = 0;
            }
            if (!finite_vector(p[i]) || !finite_vector(v[i])) return numerical();
        }
    }
    const auto metrics = measure(p, v);
    positions_.swap(p); velocities_.swap(v); metrics_ = metrics;
    return {};
}
XpbdMetrics XpbdSolver::measure(std::span<const ParticlePosition> p, std::span<const ParticleVelocity> v) const {
    XpbdMetrics m;
    m.particle_count = static_cast<std::uint32_t>(p.size());
    m.constraint_count = static_cast<std::uint32_t>(constraints_.size());
    m.color_count = static_cast<std::uint32_t>(color_offsets_.size()-1);
    m.min_height = std::numeric_limits<double>::max();
    for (std::size_t i = 0; i < p.size(); ++i) {
        const double speed2 = static_cast<double>(v[i].x)*v[i].x + static_cast<double>(v[i].y)*v[i].y + static_cast<double>(v[i].z)*v[i].z;
        m.max_speed = std::max(m.max_speed, std::sqrt(speed2));
        m.min_height = std::min(m.min_height, static_cast<double>(p[i].y));
        m.max_penetration = std::max(m.max_penetration, static_cast<double>(config_.floor_y)-p[i].y);
        if (p[i].inverse_mass > 0) {
            const auto mass = 1.0/static_cast<double>(p[i].inverse_mass);
            m.kinetic_energy += 0.5*mass*speed2;
            m.gravity_potential_energy -= mass*config_.gravity_y*(static_cast<double>(p[i].y)-config_.floor_y);
        } else m.max_pin_displacement = std::max(m.max_pin_displacement, static_cast<double>(distance(p[i], initial_[i])));
    }
    double sum = 0;
    for (const auto& c : constraints_) {
        const auto& a = p[c.a]; const auto& b = p[c.b];
        const double x = static_cast<double>(a.x)-b.x, y = static_cast<double>(a.y)-b.y, z = static_cast<double>(a.z)-b.z;
        const auto error = std::abs(std::sqrt(x*x+y*y+z*z)-c.rest_length);
        sum += error*error; m.max_constraint_error = std::max(m.max_constraint_error, error);
        m.max_relative_error = std::max(m.max_relative_error, error/c.rest_length);
        if (c.compliance > 0) m.compliant_energy += 0.5*error*error/c.compliance;
    }
    if (!constraints_.empty()) m.rms_constraint_error = std::sqrt(sum/static_cast<double>(constraints_.size()));
    return m;
}
XpbdSnapshot XpbdSolver::snapshot() const { return {config_, positions_, velocities_, constraints_, color_offsets_, metrics_}; }
}
