#include <dk/automation/RpcRuntime.hpp>
namespace dk {
namespace {
RpcEndpoint endpoint(Runtime& runtime,bool auto_guard) {
    return {[&](std::string_view method) { return runtime.has_command(method); },
        [&,auto_guard](std::string_view method,const Json& params) -> RpcCall {
            auto executed=runtime.dispatch(method,params,auto_guard);
            if (!executed) return {{},std::unexpected(executed.error())};
            return {executed->task_id.to_string(),std::move(executed->result)};
        }};
}
} // namespace
RpcOutcome dispatch_json_rpc(Runtime& runtime,const Json& request,bool auto_guard) {
    return dispatch_json_rpc(endpoint(runtime,auto_guard),request);
}
RpcOutcome dispatch_json_line(Runtime& runtime,std::string_view line,bool auto_guard) {
    return dispatch_json_line(endpoint(runtime,auto_guard),line);
}
}
