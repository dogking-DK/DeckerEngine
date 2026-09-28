#pragma once
#include <dk/assets/CpuArtifact.hpp>
#include <dk/assets/Metadata.hpp>
#include <dk/io/File.hpp>
#include <functional>
namespace dk::asset_detail {
struct EncodedCpuArtifact { std::string manifest; ByteBuffer bytes; };
[[nodiscard]] Result<EncodedCpuArtifact> encode_cpu_artifact(const ImportResult& imported, const AssetMetadata& metadata);
enum class CompileStep { validate_inputs, publish, metadata };
using CompileHook = std::function<Result<void>(CompileStep)>;
class ScopedCompileHook {
public:
    explicit ScopedCompileHook(const CompileHook& hook) noexcept;
    ~ScopedCompileHook();
    ScopedCompileHook(const ScopedCompileHook&) = delete;
    ScopedCompileHook& operator=(const ScopedCompileHook&) = delete;
private:
    const CompileHook* previous_;
};
} // namespace dk::asset_detail
