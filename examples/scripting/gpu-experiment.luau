-- dk-run --project-root ROOT --script gpu-experiment.luau --script-timeout-ms 120000
-- Requires graphics + Luau, project access, and a new output directory.
local function call(method, params)
    local r = dk.command(method, params)
    if not r.ok then error(r.error.name .. ": " .. r.error.message) end
    return r.value
end
assert(call("runtime.capabilities").simulation.experiment_export)
local edit = call("scene.new", {name = "GPU cloth experiment"})
local run = call("simulation.start", {
    guard = {document_id = edit.document_id, revision = edit.revision},
    paused = true, solver = "xpbd_gpu", fixed_dt_ns = 10000000, cloth = {seed = 42}
}).run
local n = 300
for first = 0, n - 1, 8 do
    local count = math.min(8, n - first)
    run = call("simulation.step", {run_id = run.run_id, count = count}).run
    assert(run.steps == first + count and run.fault == dk.null)
end
local exported = call("simulation.export", {
    run_id = run.run_id, expected_steps = n, output = "luau-experiment", width = 640, height = 480
})
assert(exported.steps == n and exported.simulated_time_ns == n * 10000000)
assert(call("simulation.query").run.steps == n)
call("simulation.stop", {run_id = run.run_id})
local after = call("scene.query").state
assert(after.document_id == edit.document_id and after.revision == edit.revision)
