-- M10.1: exact clock ticks in an independent world; no physics solver yet.
local function call(method, params)
    local r = dk.command(method, params)
    if not r.ok then error(r.error.name .. ": " .. r.error.message) end
    return r.value
end
call("scene.new", { name = "Fixed step example" })
local edit = call("scene.query").state
local run = call("simulation.start", {
    guard = { document_id = edit.document_id, revision = edit.revision },
    fixed_dt_ns = 10000000, paused = true,
}).run
local stepped = call("simulation.step", { run_id = run.run_id, count = 100 }).run
assert(stepped.steps == 100 and stepped.simulated_time_ns == 1000000000)
assert(call("simulation.query").run.steps == 100)
assert(call("simulation.stop", { run_id = run.run_id }).mode == "edit")
local after = call("scene.query").state
assert(after.document_id == edit.document_id and after.revision == edit.revision and after.dirty == edit.dirty)
