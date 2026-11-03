#include "ConsoleAssist.hpp"
#include "CommandFieldHelp.hpp"
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
std::string path_key(const std::string& path,const std::string& key) {
    Json::json_pointer pointer(path); pointer/="properties"; pointer/=key; return pointer.to_string();
}
bool required(const Json& schema,const std::string& key) {
    return schema.contains("required") && std::ranges::find(schema["required"],Json(key))!=schema["required"].end();
}
const Json* property(const Json* schema,const std::string& key) {
    return schema && schema->contains("properties") && (*schema)["properties"].contains(key) ? &(*schema)["properties"][key] : nullptr;
}
struct Context {
    const ConsoleNode* node=nullptr;
    const ConsoleNode* parent=nullptr;
    const Json* schema=nullptr;
    const Json* parent_schema=nullptr;
    std::string path;
    bool key=false, json=false;
};
Context locate(const ConsoleNode& node,const Json* schema,const std::string& path,std::size_t cursor,bool json,bool named_root=false) {
    for(const auto& child : node.children) {
        if(child.has_key && cursor>=child.key_begin && cursor<=child.key_end)
            return {&child,&node,property(schema,child.key),schema,path,true,json};
        if(cursor>=child.begin && cursor<=child.end) {
            const auto* next=child.has_key ? property(schema,child.key) : schema && schema->contains("items") ? &(*schema)["items"] : nullptr;
            auto found=locate(child,next,child.has_key ? path_key(path,child.key) : path+"/items",cursor,true);
            if(!found.parent) { found.parent=&node; found.parent_schema=schema; }
            return found;
        }
    }
    if(named_root && (node.children.empty() || cursor>node.children.back().end)) return {nullptr,&node,nullptr,schema,path,true,false};
    return {&node,nullptr,schema,nullptr,path,false,json};
}
Json scalar_values(const Json& schema) {
    if(schema.contains("enum")) return schema["enum"];
    auto values=Json::array();
    for(const auto& v : {Json(false),Json(true),Json(nullptr)}) if(schema::validate(schema,v)) values.push_back(v);
    return values;
}
void diagnose(ConsoleAssistance& result,const ConsoleNode& node,const Json& definition,bool relaxed=false) {
    const auto report=[&](ConsoleDiagnosticKind kind,std::size_t at,std::string message) {
        console_diagnostic(result.diagnostics,result.source.text,kind,at,bounded(std::move(message),512));
    };
    if(node.shape==ConsoleShape::missing || !node.valid) return;
    if(node.shape==ConsoleShape::scalar) {
        if(!node.complete) return;
        auto checked=schema::validate(definition,node.value);
        if(!checked) {
            bool pending=false;
            if(node.end==result.source.text.size() && node.value.is_string() && node.begin<node.end && result.source.text[node.begin]!='"' && result.source.text[node.begin]!='\'') {
                const auto word=node.value.get<std::string>();
                for(const auto& v : scalar_values(definition)) pending|=(v.is_string() ? v.get<std::string>() : v.dump()).starts_with(word);
            }
            report(pending ? ConsoleDiagnosticKind::incomplete : ConsoleDiagnosticKind::error,node.begin,checked.error().message);
        }
        return;
    }
    for(const auto& child : node.children) {
        const auto* next=child.has_key ? property(&definition,child.key) : definition.contains("items") ? &definition["items"] : nullptr;
        if(child.has_key && child.separator && child.key_complete && !next && definition.value("additionalProperties",true)==false)
            report(ConsoleDiagnosticKind::error,child.key_begin,"Unknown field: "+child.key);
        if(next) diagnose(result,child,*next);
    }
    auto shallow=definition; shallow.erase("properties"); shallow.erase("items"); shallow.erase("additionalProperties");
    if(!node.complete || relaxed) { shallow.erase("required"); shallow.erase("minItems"); }
    if(auto check=schema::validate(shallow,node.value); !check) report(ConsoleDiagnosticKind::error,node.begin,check.error().message);
    if(relaxed && definition.contains("required")) for(const auto& key : definition["required"])
        if(std::ranges::none_of(node.children,[&](const auto& child){return child.key==key.get_ref<const std::string&>() && child.separator;}))
            report(ConsoleDiagnosticKind::incomplete,node.end,"Required parameter not entered: "+key.get<std::string>());
}
void add_candidate(ConsoleAssistance& result,std::string label,std::string insert,std::string description) {
    if(insert.size()>console_input_limit) return;
    if(std::ranges::any_of(label,[](unsigned char c){return c<0x20 || c==0x7f;})) label=Json(label).dump();
    ConsoleCandidate candidate{bounded(std::move(label),256),std::move(insert),bounded(std::move(description),2048)};
    const auto less=[](const auto& a,const auto& b){return a.label!=b.label ? a.label<b.label : a.insert<b.insert;};
    auto at=std::lower_bound(result.candidates.begin(),result.candidates.end(),candidate,less);
    if(at!=result.candidates.end() && at->insert==candidate.insert) return;
    result.candidates.insert(at,std::move(candidate));
    if(result.candidates.size()>32) { result.candidates.pop_back(); result.truncated=true; }
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
ConsoleAssistance assist_console(const CommandCatalogSnapshot& catalog,const ConsoleEditState& state,ConsoleReferenceVersion references) {
    ConsoleAssistance result{state,catalog.version}; result.references=std::move(references);
    if(!valid(state)) { console_diagnostic(result.diagnostics,state.text,ConsoleDiagnosticKind::error,0,"Invalid UTF-8, position or input budget"); return result; }
    const auto structure=scan_console_structure(state.text,state.cursor); result.diagnostics=structure.diagnostics;
    const auto found=std::ranges::find(catalog.commands,structure.method,&CommandDescriptor::name);
    const CommandDescriptor* command=found==catalog.commands.end() ? nullptr : &*found;
    const auto commands=[&](bool quoted,std::size_t begin,std::size_t end) {
        result.begin=begin; result.end=end;
        if(!selected_token(state,begin,end)) return;
        const auto word=prefix(state.selection_start!=state.selection_end ? std::string_view{} : std::string_view(state.text).substr(begin,state.cursor-begin));
        if(!word) return;
        for(const auto& d : catalog.commands) if(d.name.starts_with(*word)) add_candidate(result,d.name,quoted ? Json(d.name).dump() : d.name,d.description);
    };
    if(!structure.envelope && !command && !structure.method.empty()) {
        const bool partial=std::ranges::any_of(catalog.commands,[&](const auto& d){return d.name.starts_with(structure.method);});
        console_diagnostic(result.diagnostics,state.text,partial ? ConsoleDiagnosticKind::incomplete : ConsoleDiagnosticKind::error,structure.method_begin,
            partial ? "Complete command name" : "Unknown command");
    }
    if(!structure.envelope && state.cursor>=structure.method_begin && state.cursor<=structure.method_end) {
        commands(false,structure.method_begin,structure.method_end);
        if(command) result.hint=bounded(command->name+"\n"+command->description,2048);
        return result;
    }
    Json envelope=schema::object({{"method",schema::string(1,96)},{"params",command ? command->parameters : Json{{"type","object"}}}},{"method","params"});
    const Json* definition=structure.envelope ? &envelope : command ? &command->parameters : nullptr;
    if(!definition) return result;
    diagnose(result,structure.root,*definition,!structure.envelope);
    if(std::ranges::any_of(structure.diagnostics,[](const auto& d){return d.kind==ConsoleDiagnosticKind::error;})) return result;
    auto context=locate(structure.root,definition,"",state.cursor,structure.envelope,!structure.envelope);
    const bool method=structure.envelope && !context.key && context.path=="/properties/method";
    if(structure.envelope && !command && !structure.method.empty())
        console_diagnostic(result.diagnostics,state.text,ConsoleDiagnosticKind::error,structure.method_begin,"Unknown command");
    if(method && context.node) { commands(true,context.node->begin,context.node->end); return result; }
    if(structure.envelope && context.path.starts_with("/properties/params")) context.path.erase(0,std::string_view("/properties/params").size());
    else if(structure.envelope) command=nullptr;
    if(command) result.hint=bounded(command->name+"\n"+command->description,2048);
    if(context.key) {
        if(!context.parent_schema || !context.parent_schema->contains("properties")) return result;
        result.begin=context.node ? context.node->key_begin : state.cursor; result.end=context.node ? context.node->key_end : state.cursor;
        if(!selected_token(state,result.begin,result.end)) return result;
        const auto word=prefix(state.selection_start!=state.selection_end ? std::string_view{} : std::string_view(state.text).substr(result.begin,state.cursor-result.begin));
        if(!word) return result;
        for(const auto& [key,field] : (*context.parent_schema)["properties"].items()) {
            if(!key.starts_with(*word)) continue;
            if(!context.json && (key.empty() || (!letter(key[0]) && key[0]!='_') || !std::ranges::all_of(key,name_char))) continue;
            if(context.parent && std::ranges::any_of(context.parent->children,[&](const auto& n){return &n!=context.node && n.key==key;})) continue;
            const auto path=path_key(context.path,key);
            auto hint=command_field_help(command,path,field,required(*context.parent_schema,key));
            if(context.node && context.node->key==key) result.hint=hint;
            auto insert=context.json ? Json(key).dump() : key;
            if(!context.node || !context.node->separator) insert+=context.json ? ":" : "=";
            add_candidate(result,key,std::move(insert),std::move(hint));
        }
    } else if(context.node && context.schema) {
        const auto& node=*context.node; const auto& field=*context.schema;
        result.hint=command_field_help(command,context.path,field,context.parent_schema && required(*context.parent_schema,node.key));
        if(node.shape==ConsoleShape::object || node.shape==ConsoleShape::array) return result;
        result.begin=node.begin; result.end=node.end;
        if(!selected_token(state,result.begin,result.end)) return result;
        const auto word=prefix(state.selection_start!=state.selection_end ? std::string_view{} : std::string_view(state.text).substr(result.begin,state.cursor-result.begin));
        if(!word) return result;
        for(const auto& v : scalar_values(field)) {
            const auto label=v.is_string() ? v.get<std::string>() : v.dump();
            if(label.starts_with(*word) && schema::validate(field,v)) add_candidate(result,label,v.dump(),result.hint);
        }
        if(command && command->help) {
            const auto& fields=command->help->at("fields");
            if(fields.contains(context.path)) result.provider=fields[context.path].value("completion_provider",std::string{});
        }
        result.search=*word;
        if(result.provider=="assets.reference" && context.parent && context.parent->value.contains("kind")) {
            const auto& kind=context.parent->value["kind"];
            result.kind_filter=kind.is_string() ? kind.get<std::string>() : "invalid";
        }
        result.loading=!result.provider.empty();
    }
    return result;
}
void add_console_reference_page(ConsoleAssistance& result,const ConsoleReferencePage& page) {
    if(result.references!=page.version || result.provider.empty()) return;
    for(const auto& record : page.records) {
        if(!result.kind_filter.empty() && record.kind!=result.kind_filter) continue;
        if(!record.name.starts_with(result.search) && !record.id.starts_with(result.search)) continue;
        auto name=bounded(record.name,150);
        if(std::ranges::any_of(name,[](unsigned char c){return c<0x20 || c==0x7f;})) name=bounded(Json(name).dump(),150);
        const auto label=name+" ["+record.kind+" "+record.id+"]";
        add_candidate(result,label,Json(record.id).dump(),result.hint);
    }
    result.loading=page.has_more;
}
Result<ConsoleEditState> accept_console_candidate(const ConsoleAssistance& assistance, std::size_t index,
    const ConsoleEditState& current, CommandCatalogVersion version, ConsoleReferenceVersion references) {
    const auto fail=[](std::string message)->Result<ConsoleEditState> { return std::unexpected(Error{ErrorCode::invalid_argument,std::move(message)}); };
    if (assistance.source!=current || assistance.catalog!=version || assistance.references!=references) return fail("Completion is stale; input unchanged.");
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
