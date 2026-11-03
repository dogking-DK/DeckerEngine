#include "ConsoleStructure.hpp"
#include "ConsoleHistory.hpp"
#include "ConsoleLexical.hpp"
#include <algorithm>
#include <set>

namespace dk::editor::detail {
void console_diagnostic(std::vector<ConsoleDiagnostic>& out, std::string_view text, ConsoleDiagnosticKind kind, std::size_t at, std::string message) {
    if(out.size()>=16) return;
    if(message.size()>512) {
        std::size_t end=509;
        while((static_cast<unsigned char>(message[end])&0xc0)==0x80) --end;
        message.resize(end); message+="...";
    }
    at=std::min(at,text.size()); std::size_t column=1;
    for(std::size_t i=0;i<at;++i) if((static_cast<unsigned char>(text[i])&0xc0)!=0x80) ++column;
    out.push_back({kind,at,column,std::move(message)});
}
namespace {
class Scanner {
    std::string_view text_;
    std::size_t at_=0, cursor_;
    bool stopped_=false;
    ConsoleStructure result_;
    void skip() { while(at_<text_.size() && space(text_[at_])) ++at_; }
    void problem(ConsoleDiagnosticKind kind, std::size_t at, std::string message) {
        console_diagnostic(result_.diagnostics,text_,kind,at,std::move(message));
        if(kind==ConsoleDiagnosticKind::error) stopped_=true;
    }
    void incomplete(std::size_t at,std::string message) { problem(ConsoleDiagnosticKind::incomplete,at,std::move(message)); }
    void error(std::size_t at,std::string message) { problem(ConsoleDiagnosticKind::error,at,std::move(message)); }
    bool quote() {
        const auto start=at_++; const auto q=text_[start];
        while(at_<text_.size()) {
            const auto c=text_[at_++];
            if(c==q) return true;
            if(q=='"' && c=='\\' && at_<text_.size()) ++at_;
        }
        incomplete(start,"Unclosed string"); return false;
    }
    void scalar(ConsoleNode& n, bool json) {
        n.shape=ConsoleShape::scalar; const auto first=text_[at_];
        if(first=='"' || (!json && first=='\'')) n.complete=quote();
        else {
            while(at_<text_.size() && !space(text_[at_]) && (!json || std::string_view(",]}").find(text_[at_])==std::string_view::npos)) {
                if(bare_forbidden(text_[at_])) { error(at_,"Invalid value delimiter"); n.valid=false; break; }
                ++at_;
            }
            n.complete=at_>n.begin;
            if(!n.complete) { n.valid=false; error(at_,"Expected a JSON value"); }
        }
        n.end=at_;
        if(!n.complete || !n.valid) return;
        const auto token=text_.substr(n.begin,n.end-n.begin);
        if(json && first!='"') {
            Json value;
            try { value=Json::parse(token,nullptr,false); }
            catch(const Json::exception&) { n.valid=false; error(n.begin,"Numeric precision/range overflow"); return; }
            if(value.is_discarded()) {
                bool pending=false;
                if(at_==text_.size()) {
                    for(const auto* literal : {"true","false","null"}) pending|=std::string_view(literal).starts_with(token);
                    if(!token.empty() && (digit(token[0]) || token[0]=='-'))
                        pending|=!Json::parse(std::string(token)+"0",nullptr,false).is_discarded();
                }
                n.complete=false; n.valid=pending;
                if(pending) incomplete(n.begin,"Incomplete JSON value"); else error(n.begin,"Invalid JSON value");
                return;
            }
        }
        auto parsed=parse_console_command("m x="+std::string(token));
        if(!parsed) { n.valid=false; error(n.begin,"Invalid value or numeric precision/range overflow"); }
        else n.value=std::move(parsed->params["x"]);
    }
    ConsoleNode value(bool json,std::size_t depth) {
        ConsoleNode n; n.begin=n.end=at_;
        if(depth>64) { n.valid=false; error(at_,"JSON depth exceeds 64"); return n; }
        if(at_==text_.size()) { incomplete(at_,"Expected a value"); return n; }
        const auto first=text_[at_];
        if(first!='{' && first!='[') { scalar(n,json); return n; }
        n.shape=first=='{' ? ConsoleShape::object : ConsoleShape::array;
        n.value=first=='{' ? Json::object() : Json::array();
        ++at_; skip(); std::set<std::string> keys; bool after_comma=false;
        while(!stopped_) {
            if(at_==text_.size()) {
                ConsoleNode placeholder; placeholder.begin=placeholder.end=placeholder.key_begin=placeholder.key_end=at_;
                placeholder.has_key=first=='{'; n.children.push_back(std::move(placeholder));
                incomplete(at_,first=='{' ? "Incomplete object" : "Incomplete array"); break;
            }
            if(text_[at_]==(first=='{' ? '}' : ']')) {
                if(after_comma) { error(at_,"Trailing comma"); break; }
                ++at_; n.complete=true; break;
            }
            ConsoleNode child;
            if(first=='{') {
                child.has_key=true; child.key_begin=at_;
                if(text_[at_]!='"') { error(at_,"JSON object keys require double quotes"); break; }
                child.key_complete=quote(); child.key_end=at_; child.begin=child.end=at_;
                if(!child.key_complete) { n.children.push_back(std::move(child)); break; }
                auto decoded=Json::parse(text_.substr(child.key_begin,child.key_end-child.key_begin),nullptr,false);
                if(!decoded.is_string()) { error(child.key_begin,"Invalid key escape"); break; }
                child.key=decoded.get<std::string>();
                if(!keys.insert(child.key).second) console_diagnostic(result_.diagnostics,text_,ConsoleDiagnosticKind::error,child.key_begin,"Duplicate field: "+child.key);
                skip();
                if(at_==text_.size()) { incomplete(at_,"Expected ':' and a value"); n.children.push_back(std::move(child)); break; }
                if(text_[at_]!=':') { error(at_,"Expected ':'"); n.children.push_back(std::move(child)); break; }
                ++at_; child.separator=true; skip();
            }
            auto v=value(true,depth+1);
            v.has_key=child.has_key; v.key=std::move(child.key); v.key_begin=child.key_begin; v.key_end=child.key_end;
            v.key_complete=child.key_complete; v.separator=child.separator;
            if(v.valid && v.complete) {
                if(first=='{') n.value[v.key]=v.value; else n.value.push_back(v.value);
            }
            const bool complete=v.complete; n.valid&=v.valid;
            n.children.push_back(std::move(v));
            if(!complete || stopped_) break;
            skip(); after_comma=false;
            if(at_<text_.size() && text_[at_]==',') { ++at_; skip(); after_comma=true; continue; }
            if(at_==text_.size()) { incomplete(at_,"Unclosed JSON container"); break; }
            if(text_[at_]!=(first=='{' ? '}' : ']')) { error(at_,"Expected ',' or closing bracket"); break; }
        }
        n.end=at_; return n;
    }
public:
    Scanner(std::string_view text,std::size_t cursor):text_(text),cursor_(cursor) {}
    ConsoleStructure scan() {
        if(auto invalid=console_text_problem(text_)) { error(0,*invalid); return std::move(result_); }
        skip(); result_.method_begin=at_;
        if(at_<text_.size() && text_[at_]=='{') {
            result_.envelope=true; result_.root=value(true,0); skip();
            if(at_<text_.size() && !stopped_) error(at_,"Unexpected text after JSON envelope");
            for(const auto& n : result_.root.children) if(n.key=="method" && n.value.is_string()) {
                result_.method=n.value.get<std::string>(); result_.method_begin=n.begin; result_.method_end=n.end;
            }
            return std::move(result_);
        }
        while(at_<text_.size() && !space(text_[at_])) ++at_;
        result_.method_end=at_; result_.method=text_.substr(result_.method_begin,at_-result_.method_begin);
        auto& root=result_.root; root.shape=ConsoleShape::object; root.begin=at_; root.value=Json::object();
        std::set<std::string> keys;
        while(!stopped_) {
            skip(); if(at_==text_.size()) break;
            ConsoleNode child; child.has_key=true; child.key_begin=at_;
            if(!letter(text_[at_]) && text_[at_]!='_') { error(at_,"Expected a named parameter"); break; }
            while(at_<text_.size() && name_char(text_[at_])) ++at_;
            child.key_end=at_; child.key=text_.substr(child.key_begin,at_-child.key_begin); child.key_complete=true;
            if(!keys.insert(child.key).second) console_diagnostic(result_.diagnostics,text_,ConsoleDiagnosticKind::error,child.key_begin,"Duplicate parameter: "+child.key);
            skip(); child.begin=child.end=at_;
            if(at_==text_.size()) { incomplete(at_,"Expected '=' and a value"); root.children.push_back(std::move(child)); break; }
            if(text_[at_]!='=') {
                if(cursor_>=child.key_begin && cursor_<=child.key_end) { root.children.push_back(std::move(child)); continue; }
                error(at_,"Expected '='"); root.children.push_back(std::move(child)); break;
            }
            ++at_; skip(); auto v=value(false,2);
            v.has_key=true; v.key=std::move(child.key); v.key_begin=child.key_begin; v.key_end=child.key_end; v.key_complete=true; v.separator=true;
            if(v.valid && v.complete) root.value[v.key]=v.value;
            const bool complete=v.complete; root.children.push_back(std::move(v));
            if(!complete) break;
            if(at_<text_.size() && !space(text_[at_])) error(at_,"Expected whitespace after value");
        }
        root.end=text_.size(); root.complete=!stopped_ && std::ranges::all_of(root.children,[](const auto& n){return n.complete;});
        return std::move(result_);
    }
};
}
ConsoleStructure scan_console_structure(std::string_view text,std::size_t cursor) {
    try { return Scanner(text,cursor).scan(); }
    catch(const Json::exception&) {
        ConsoleStructure result; console_diagnostic(result.diagnostics,text,ConsoleDiagnosticKind::error,0,"Invalid JSON or out-of-range number"); return result;
    }
}
}
