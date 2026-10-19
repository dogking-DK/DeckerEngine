-- Copy tests/fixtures/render-disk to ROOT. Requires capture, path tracing, Luau and an RT GPU.
-- dk-run --project-root ROOT --script pathtrace-capture.luau --script-timeout-ms 150000
local function call(method, params)
    local response = dk.command(method, params)
    if not response.ok then error(response.error.name .. ": " .. response.error.message) end
    return response.value
end
local state = call("scene.load", {manifest = "project.json"})
assert(call("runtime.capabilities").pathtrace_capture, "Path tracing capture was not built")
local ticket = call("render.capture", {
    guard = {document_id = state.document_id, revision = state.revision},
    renderer = "pathtrace", format = "linear_json", output = "path-luau.json",
    width = 48, height = 32, validation = "required",
    camera = {projection = "orthographic", ortho_height = 3, eye = {0, 0, -2}, target = {0, 0, 0}, near = 0.05, far = 10000},
    pathtrace = {version = 1, target_spp = 3, seed = 7, max_bounces = 4, environment_rgb = {0.3, 0.4, 0.6}}
})
assert(ticket.provenance.actual_spp == 0 and ticket.provenance.input_fingerprint == dk.null)
local job
repeat
    job = call("jobs.wait", {id = ticket.job_id, timeout_ms = 1000}).job
    assert(job.progress.completed <= job.progress.submitted and job.progress.submitted <= 3)
until job.state == "succeeded" or job.state == "failed" or job.state == "cancelled"
assert(job.state == "succeeded", "Path capture failed")
assert(job.progress.completed == 3 and job.result.provenance.actual_spp == 3)
assert(job.result.document_id == state.document_id and job.result.revision == state.revision)
