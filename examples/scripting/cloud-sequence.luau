-- Disposable render-disk project. Requires Clouds, capture and Luau.
local function call(method, params)
    local r = dk.command(method, params)
    if not r.ok then error(r.error.name .. ": " .. r.error.message) end
    return r.value
end
call("scene.load", {manifest = "project.json"})
local query = call("environment.query")
local environment = call("environment.preset", {name = "sparse"}).environment
environment.atmosphere.planet_center_m[2] -= 500
environment.sun.direction = {0.2, 0.8, -0.3}
environment.clouds.weather_resolution = 16
environment.clouds.base_resolution = 16
environment.clouds.detail_resolution = 8
environment.clouds.coverage = 0.85
environment.clouds.extinction_per_m = 0.0003
local state = call("environment.set", {
    guard = {document_id = query.state.document_id, revision = query.state.revision}, environment = environment
}).state
local camera = {eye = {0, 0, -2}, target = {0, 0, 0}, up = {0, 1, 0}, fov_y = 65, near = 1, far = 10000}
local ticket = call("render.sequence", {
    guard = {document_id = state.document_id, revision = state.revision}, output = "cloud-luau.json", width = 16, height = 16,
    render_options = {mode = "temporal", cloud_shadows = true, shadow_resolution = 32}, warmup_frames = 2,
    samples = {{camera = camera, time_seconds = 0, camera_cut = false}, {camera = camera, time_seconds = 0.03, camera_cut = false}},
    validation = "required"
})
local job
repeat
    job = call("jobs.wait", {id = ticket.job_id, timeout_ms = 1000}).job
until job.state == "succeeded" or job.state == "failed" or job.state == "cancelled"
assert(job.state == "succeeded", "Cloud sequence failed")
assert(job.result.kind == "render_sequence" and job.result.provenance.quality == "cloud_temporal_v1")
assert(job.result.provenance.warmup_frames == 2 and job.result.provenance.sample_count == 2)
