#include "CommandFieldHelp.hpp"
namespace dk::editor::detail {
namespace {
std::string bounded(std::string value,std::size_t limit) {
    if(value.size()<=limit) return value;
    auto end=limit-3; while((static_cast<unsigned char>(value[end])&0xc0)==0x80) --end;
    value.resize(end); return value+"...";
}
}
std::string command_field_help(const CommandDescriptor* command,const std::string& path,const Json& field,bool required) {
    std::string hint=(command ? command->name : "Console envelope")+"  "+path+(required ? " (required)" : " (optional)");
    for(const auto* name : {"type","minimum","maximum","exclusiveMinimum","exclusiveMaximum","minLength","maxLength","minItems","maxItems","enum"})
        if(field.contains(name)) hint+="\n"+std::string(name)+": "+bounded(field[name].dump(),512);
    if(field.contains("items")) for(const auto* name : {"type","minimum","maximum","exclusiveMinimum","exclusiveMaximum","enum"})
        if(field["items"].contains(name)) hint+="\nitems."+std::string(name)+": "+bounded(field["items"][name].dump(),256);
    if(command && command->help) {
        const auto& fields=command->help->at("fields");
        if(fields.contains(path)) for(const auto* name : {"label","description","unit","default","examples"}) {
            const auto& entry=fields[path];
            if(entry.contains(name)) hint+="\n"+std::string(name)+": "+bounded(entry[name].is_string() ? entry[name].get<std::string>() : entry[name].dump(),512);
        }
    }
    return bounded(std::move(hint),2048);
}
}
