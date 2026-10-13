-- Copy tests/fixtures/render-disk to ROOT first. Requires atmosphere, capture and Luau.
-- dk-run --project-root ROOT --script environment-capture.luau --script-timeout-ms 120000
local function call(method, params)
    local r = dk.command(method, params)
    if not r.ok then error(r.error.name .. ": " .. r.error.message) end
    return r.value
end
call("scene.load", {manifest = "project.json"})
local query = call("environment.query")
local environment = query.environment
environment.enabled = true
environment.atmosphere.planet_center_m = {0, -environment.atmosphere.ground_radius_m - 1000, 0}
environment.sun.direction = {0, 0.2, 1}
environment.sun.irradiance = {4, 4, 4}
local state = call("environment.set", {
    guard = {document_id = query.state.document_id, revision = query.state.revision}, environment = environment
}).state
local ticket = call("render.capture", {
    guard = {document_id = state.document_id, revision = state.revision}, output = "environment-luau.ppm",
    width = 128, height = 96, exposure = 1, profile = "unlit_preview", validation = "required",
    camera = {eye = {0, 0, 4}, target = {0, 0, 0}, fov_y = 65, near = 1, far = 10000}
})
assert(ticket.provenance.shader_fingerprint == dk.null and ticket.provenance.time_seconds == 0)
local job
repeat
    job = call("jobs.wait", {id = ticket.job_id, timeout_ms = 1000}).job
until job.state == "succeeded" or job.state == "failed" or job.state == "cancelled"
assert(job.state == "succeeded", "Environment capture failed")
assert(job.result.revision == state.revision and job.result.provenance.quality == "atmosphere_v1")
assert(string.sub(job.result.provenance.shader_fingerprint, 1, 8) == "fnv1a64:")
