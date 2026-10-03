#pragma once
#include <dk/commands/CommandRegistry.hpp>
#include <dk/jobs/JobQueue.hpp>
namespace dk {
struct JobCommands {
    std::function<Result<JobSnapshot>(JobId)> get;
    std::function<Result<JobWait>(JobId,std::chrono::milliseconds)> wait;
    std::function<Result<JobCancel>(JobId)> cancel;
};
[[nodiscard]] Result<void> register_job_commands(CommandRegistry&, JobCommands, Json result_schema);
}
