#pragma once
#include <dk/automation/JsonRpc.hpp>
#include <dk/runtime/Runtime.hpp>
namespace dk {
[[nodiscard]] RpcOutcome dispatch_json_rpc(Runtime&,const Json&,bool auto_guard=false);
[[nodiscard]] RpcOutcome dispatch_json_line(Runtime&,std::string_view,bool auto_guard=false);
}
