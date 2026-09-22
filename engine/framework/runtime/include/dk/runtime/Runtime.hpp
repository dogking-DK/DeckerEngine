#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/services/SceneService.hpp>

namespace dk
{
class Runtime final
{
  public:
    [[nodiscard]] static Result<std::unique_ptr<Runtime>> create(const std::filesystem::path &root);
    [[nodiscard]] Result<Json> dispatch(std::string_view method, const Json &parameters,
                                        bool auto_guard = false);
    [[nodiscard]] bool has_command(std::string_view method) const;

  private:
    explicit Runtime(std::unique_ptr<SceneService> service) : service_{std::move(service)} {}
    std::unique_ptr<SceneService> service_;
    CommandRegistry commands_;
    bool dispatching_ = false;
};
} // namespace dk
