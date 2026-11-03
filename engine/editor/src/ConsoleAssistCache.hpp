#pragma once
#include "ConsoleAssist.hpp"
#include <functional>

namespace dk::editor::detail {
class ConsoleAssistCache final {
public:
    using ReadPage=std::function<Result<ConsoleReferencePage>(std::string_view,const ConsoleReferenceVersion&,std::size_t,std::size_t)>;
    bool observe(const ConsoleEditState&,CommandCatalogVersion,ConsoleReferenceVersion,std::uint64_t now_ms);
    void poll(const CommandCatalogSnapshot&,const ReadPage&,std::uint64_t now_ms,bool explicit_tab=false);
    // Publication gate also protects future deferred providers; late work is discarded.
    bool publish(ConsoleAssistance,std::uint64_t request);
    [[nodiscard]] const std::optional<ConsoleAssistance>& result() const { return result_; }
    [[nodiscard]] std::uint64_t request_id() const { return request_; }
    [[nodiscard]] std::uint64_t publication() const { return publication_; }
private:
    ConsoleEditState source_;
    CommandCatalogVersion catalog_;
    ConsoleReferenceVersion references_;
    std::optional<ConsoleAssistance> result_;
    std::uint64_t request_=0, publication_=0, deadline_=0, last_page_=UINT64_MAX;
    std::size_t offset_=0;
    bool settled_=false;
};
}
