#pragma once
#include <dk/core/Result.hpp>
#include <stdexcept>
namespace dk::editor::detail {
template<class T,class E> T take(std::expected<T,E>&& result) {
    if (!result) {
        if constexpr (std::is_same_v<E,Error>) {
            std::string message=result.error().message;
            for (const auto& context : result.error().context) message+=" / "+context;
            throw std::runtime_error(message);
        } else throw std::runtime_error("Memory operation failed");
    }
    return std::move(*result);
}
inline void check(Result<void> result) { if (!result) throw std::runtime_error(result.error().message); }
}
