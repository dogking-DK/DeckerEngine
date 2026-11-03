#include "ConsoleAssistCache.hpp"

namespace dk::editor::detail {
bool ConsoleAssistCache::observe(const ConsoleEditState& source,CommandCatalogVersion catalog,ConsoleReferenceVersion references,std::uint64_t now) {
    if(request_ && source==source_ && catalog==catalog_ && references==references_) return false;
    source_=source; catalog_=catalog; references_=std::move(references);
    ++request_; ++publication_; result_.reset(); deadline_=now+100; offset_=0; settled_=false; last_page_=UINT64_MAX;
    return true;
}
bool ConsoleAssistCache::publish(ConsoleAssistance result,std::uint64_t request) {
    if(request!=request_ || result.source!=source_ || result.catalog!=catalog_ || result.references!=references_) return false;
    result.request_id=request; result_=std::move(result); ++publication_; return true;
}
void ConsoleAssistCache::poll(const CommandCatalogSnapshot& catalog,const ReadPage& read,std::uint64_t now,bool explicit_tab) {
    if(!request_ || catalog.version!=catalog_) return;
    if(!settled_ && (now>=deadline_ || explicit_tab)) {
        auto result=assist_console(catalog,source_,references_);
        settled_=now>=deadline_;
        if(!settled_) result.diagnostics.clear();
        publish(std::move(result),request_); offset_=0;
    }
    if(!settled_ || !result_ || !result_->loading || now==last_page_) return;
    last_page_=now;
    auto page=read(result_->provider,references_,offset_,128);
    if(!page) {
        auto next=*result_; next.loading=false; next.hint+="\n"+page.error().message;
        publish(std::move(next),request_); return;
    }
    if(page->version!=references_ || page->offset!=offset_ || page->records.size()>128 ||
        (page->has_more && page->records.empty())) return;
    auto next=*result_; add_console_reference_page(next,*page); offset_+=page->records.size();
    if(!next.loading && next.candidates.empty()) next.hint+="\nNo matching registered identities.";
    publish(std::move(next),request_);
}
}
