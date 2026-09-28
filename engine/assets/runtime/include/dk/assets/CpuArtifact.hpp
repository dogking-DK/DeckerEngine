#pragma once
#include <dk/assets/CpuData.hpp>
#include <dk/core/Result.hpp>
#include <filesystem>
namespace dk {
struct InputFingerprint { String path, digest; std::uint64_t bytes = 0; };
struct CpuArtifact { CpuAsset data; String source; Vector<InputFingerprint> inputs; };
// Complete validation before returning owned values. Requires the current persistent memory domain.
[[nodiscard]] Result<CpuArtifact> load_cpu_artifact(const std::filesystem::path& directory);
} // namespace dk
