#include "ConsoleAssist.hpp"
#include "ConsoleHistory.hpp"
#include "ConsoleLexical.hpp"
#include <algorithm>
#include <limits>
#include <set>

namespace dk::editor::detail {
namespace {
std::string bounded(std::string value, std::size_t limit) {
    if (value.size() <= limit) return value;
    auto end=limit-3;
    while ((static_cast<unsigned char>(value[end])&0xc0)==0x80) --end;
    value.resize(end); return value+"...";
}
bool boundary(std::string_view text, std::size_t offset) {
    return offset<=text.size() && (offset==text.size() || (static_cast<unsigned char>(text[offset])&0xc0)!=0x80);
}
bool valid(const ConsoleEditState& state) {
    return !console_text_problem(state.text) && boundary(state.text,state.cursor) &&
        boundary(state.text,state.selection_start) && boundary(state.text,state.selection_end);
}
struct Argument {
    std::string key;
    std::size_t start=0, key_end=0, value=0, end=0;
    bool equals=false, nested=false;
};
// Locate top-level spans, including unfinished tokens. Complete values still use
// the strict parser; this scanner never interprets nested JSON fields as keys.
bool arguments(std::string_view s, std::size_t at, std::size_t cursor, std::vector<Argument>& result) {
    while (at<s.size()) {
        while (at<s.size() && space(s[at])) ++at;
        if (at==s.size()) break;
        Argument a; a.start=at;
        if (!letter(s[at]) && s[at]!='_') return false;
        while (at<s.size() && name_char(s[at])) ++at;
        a.key_end=at; a.key=s.substr(a.start,at-a.start);
        while (at<s.size() && space(s[at])) ++at;
        if (at==s.size()) { a.end=a.key_end; result.push_back(a); return true; }
        if (s[at]!='=') {
            if (cursor<a.start || cursor>a.key_end) return false;
            a.end=a.key_end; result.push_back(a); at=a.key_end; continue;
        }
        ++at;
        a.equals=true;
        while (at<s.size() && space(s[at])) ++at;
        a.value=at;
        if (at==s.size()) { a.end=at; result.push_back(a); return true; }
        const auto first=s[at];
        bool complete=true;
        if (first=='\'' || first=='"') {
            ++at; complete=false;
            while (at<s.size()) {
                const auto c=s[at++];
                if (c==first) { complete=true; break; }
                if (first=='"' && c=='\\' && at<s.size()) ++at;
            }
        } else if (first=='{' || first=='[') {
            a.nested=true;
            std::vector<char> stack;
            bool quoted=false, escaped=false;
            do {
                const auto c=s[at++];
                if (quoted) {
                    if (escaped) escaped=false;
                    else if (c=='\\') escaped=true;
                    else if (c=='"') quoted=false;
                } else if (c=='"') quoted=true;
                else if (c=='{' || c=='[') { stack.push_back(c); if(stack.size()>64) return false; }
                else if (c=='}' || c==']') {
                    if (stack.empty() || stack.back()!=(c=='}' ? '{' : '[')) return false;
                    stack.pop_back();
                }
            } while (at<s.size() && !stack.empty());
            complete=stack.empty();
        } else {
            while (at<s.size() && !space(s[at])) { if(bare_forbidden(s[at])) return false; ++at; }
        }
        a.end=at;
        if (complete && !parse_console_command("m x="+std::string{s.substr(a.value,at-a.value)})) return false;
        result.push_back(a);
        if (at<s.size() && !space(s[at])) return false;
    }
    return true;
}
std::string field_hint(const CommandDescriptor& command, const std::string& key, const Json& field) {
    std::string hint=command.name+"  "+key;
    const auto& parameters=command.parameters;
    const auto required=parameters.find("required");
    hint+=required!=parameters.end() && std::ranges::find(*required,Json(key))!=required->end() ? " (required)" : " (optional)";
    for (const auto* name : {"type","minimum","maximum","minLength","maxLength","minItems","maxItems","enum"})
        if (field.contains(name)) hint+="\n"+std::string(name)+": "+bounded(field[name].dump(),512);
    if (command.help) {
        Json::json_pointer pointer; pointer/="properties"; pointer/=key;
        const auto& fields=command.help->at("fields");
        if (fields.contains(pointer.to_string())) {
            const auto& entry=fields[pointer.to_string()];
            for (const auto* name : {"label","description","unit","default","examples"})
                if (entry.contains(name)) hint+="\n"+std::string(name)+": "+bounded(entry[name].is_string() ? entry[name].get<std::string>() : entry[name].dump(),512);
        }
    }
    return bounded(std::move(hint),2048);
}
bool selected_token(const ConsoleEditState& s, std::size_t begin, std::size_t end) {
    return s.selection_start==s.selection_end ||
        (std::min(s.selection_start,s.selection_end)==begin && std::max(s.selection_start,s.selection_end)==end &&
            (s.cursor==begin || s.cursor==end));
}
std::optional<std::string> prefix(std::string_view s) {
    if (s.empty()) return std::string{};
    if (s.front()=='\'') {
        s.remove_prefix(1); if (!s.empty() && s.back()=='\'') s.remove_suffix(1);
        return std::string{s};
    }
    if (s.front()=='"') {
        // Try complete first, then an unfinished string. Never trim escapes by hand.
        auto value=Json::parse(s,nullptr,false);
        if (value.is_discarded()) value=Json::parse(std::string{s}+"\"",nullptr,false);
        if (!value.is_string()) return {};
        return value.get<std::string>();
    }
    return std::string{s};
}
}
ConsoleAssistance assist_console(const CommandCatalogSnapshot& catalog, const ConsoleEditState& state) {
    ConsoleAssistance result{state,catalog.version};
    if (!valid(state)) return result;
    const std::string_view text=state.text;
    auto start=text.find_first_not_of(" \t");
    if (start==std::string_view::npos) start=state.cursor;
    if (start<text.size() && text[start]=='{') return result;
    auto method_end=start;
    while (method_end<text.size() && !space(text[method_end])) {
        if (!name_char(text[method_end]) && text[method_end]!='.') return result;
        ++method_end;
    }
    const auto add=[&](std::string label, std::string insert, std::string description) {
        if (insert.size()>console_input_limit) return;
        if (std::ranges::any_of(label,[](unsigned char c) { return c<0x20 || c==0x7f; })) label=Json(label).dump();
        ConsoleCandidate candidate{bounded(std::move(label),256),std::move(insert),bounded(std::move(description),2048)};
        const auto less=[](const auto& a,const auto& b) { return a.label!=b.label ? a.label<b.label : a.insert<b.insert; };
        auto at=std::lower_bound(result.candidates.begin(),result.candidates.end(),candidate,less);
        if (at!=result.candidates.end() && at->insert==candidate.insert) return;
        result.candidates.insert(at,std::move(candidate));
        if (result.candidates.size()>32) { result.candidates.pop_back(); result.truncated=true; }
    };
    if (state.cursor>=start && state.cursor<=method_end) {
        result.begin=start; result.end=method_end;
        if (!selected_token(state,start,method_end)) return result;
        const auto word=state.selection_start!=state.selection_end ? std::string_view{} : text.substr(start,state.cursor-start);
        for (const auto& command : catalog.commands) if (command.name.starts_with(word)) {
            add(command.name,command.name,command.description);
            if (command.name==text.substr(start,method_end-start)) result.hint=bounded(command.name+"\n"+command.description,2048);
        }
    } else {
        const auto found=std::ranges::find(catalog.commands,text.substr(start,method_end-start),&CommandDescriptor::name);
        if (found==catalog.commands.end()) return result;
        result.hint=bounded(found->name+"\n"+found->description,2048);
        const auto& properties=found->parameters.value("properties",Json::object());
        std::vector<Argument> args;
        if (!arguments(text,method_end,state.cursor,args)) return result;
        const Argument* active=nullptr;
        for (const auto& arg : args) if (state.cursor>=arg.start && state.cursor<=arg.end) { active=&arg; break; }
        if (active && active->equals && state.cursor>active->key_end) {
            if (!properties.contains(active->key)) return result;
            result.hint=field_hint(*found,active->key,properties[active->key]);
            if (state.cursor<active->value || active->nested || !selected_token(state,active->value,active->end)) return result;
            result.begin=active->value; result.end=active->end;
            const auto word=prefix(state.selection_start!=state.selection_end ? std::string_view{} : text.substr(active->value,state.cursor-active->value));
            if (!word) return result;
            const auto& field=properties[active->key];
            auto values=field.value("enum",Json::array());
            if (!field.contains("enum")) {
                for (const auto& v : {Json(false),Json(true),Json(nullptr)}) if (schema::validate(field,v)) values.push_back(v);
            }
            for (const auto& value : values) {
                const auto label=value.is_string() ? value.get<std::string>() : value.dump();
                if (label.starts_with(*word) && schema::validate(field,value)) add(label,value.dump(),result.hint);
            }
        } else {
            if (active) { result.begin=active->start; result.end=active->key_end; }
            else { result.begin=state.cursor; result.end=state.cursor; }
            if (!selected_token(state,result.begin,result.end)) return result;
            const auto word=state.selection_start!=state.selection_end ? std::string_view{} : text.substr(result.begin,state.cursor-result.begin);
            for (const auto& [key,field] : properties.items()) {
                if (key.empty() || (!letter(key[0]) && key[0]!='_') || !std::ranges::all_of(key,name_char)) continue;
                if (!key.starts_with(word)) continue;
                if (std::ranges::any_of(args,[&](const auto& a) { return &a!=active && a.key==key; })) continue;
                auto hint=field_hint(*found,key,field);
                if (active && active->key==key) result.hint=hint;
                add(key,key+(active && active->equals ? "" : "="),std::move(hint));
            }
        }
    }
    return result;
}
Result<ConsoleEditState> accept_console_candidate(const ConsoleAssistance& assistance, std::size_t index,
    const ConsoleEditState& current, CommandCatalogVersion version) {
    const auto fail=[](std::string message)->Result<ConsoleEditState> { return std::unexpected(Error{ErrorCode::invalid_argument,std::move(message)}); };
    if (assistance.source!=current || assistance.catalog!=version) return fail("Completion is stale; input unchanged.");
    if (!valid(current) || index>=assistance.candidates.size() || assistance.begin>assistance.end ||
        !boundary(current.text,assistance.begin) || !boundary(current.text,assistance.end)) return fail("Invalid completion range; input unchanged.");
    const auto& insert=assistance.candidates[index].insert;
    if (current.text.size()-(assistance.end-assistance.begin)+insert.size()>console_input_limit || console_text_problem(insert))
        return fail("Completion exceeds 8192 UTF-8 bytes or contains invalid text; input unchanged.");
    if (current.revision==std::numeric_limits<std::uint64_t>::max()) return fail("Input revision exhausted; input unchanged.");
    auto next=current;
    next.text.replace(assistance.begin,assistance.end-assistance.begin,insert);
    next.cursor=next.selection_start=next.selection_end=assistance.begin+insert.size();
    ++next.revision;
    return next;
}
}
