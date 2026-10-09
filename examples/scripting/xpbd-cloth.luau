-- CPU reference: an 8x8 cloth, pinned along row zero, with a frictionless floor.
local function call(method, params)
    local r = dk.command(method, params)
    if not r.ok then error(r.error.name .. ": " .. r.error.message) end
    return r.value
end
call("scene.new", {name = "XPBD cloth reference"})
local edit = call("scene.query").state
local run = call("simulation.start", {
    guard = {document_id = edit.document_id, revision = edit.revision},
    solver = "xpbd_cpu", paused = true, fixed_dt_ns = 10000000,
    cloth = {seed = 42},
}).run
local result = call("simulation.step", {run_id = run.run_id, count = 300}).run
assert(result.steps == 300 and result.simulated_time_ns == 3000000000)
assert(result.metrics.particle_count == 64 and result.metrics.constraint_count == 210)
assert(result.metrics.max_pin_displacement == 0 and result.metrics.max_penetration <= 0.000001)
assert(result.metrics.max_relative_error < 0.08)
local particles = call("simulation.particles", {run_id = run.run_id, limit = 128})
assert(particles.steps == 300 and #particles.particles == 64 and not particles.has_more)
assert(call("scene.query").state.entity_count == 0)
call("simulation.stop", {run_id = run.run_id})
local after = call("scene.query").state
assert(after.document_id == edit.document_id and after.revision == edit.revision and after.dirty == edit.dirty)
