#pragma once
#include <dk/services/SimulationService.hpp>
#include <dk/graphics/Resources.hpp>

namespace dk {
// An already completed image, consumed once by the presentation owner. No CPU pixels.
struct SimulationPreview {
    SimulationId run_id;
    SimulationView view;
    std::uint64_t steps = 0;
    graphics::ImageTransfer image;
};
}
