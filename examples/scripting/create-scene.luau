-- Run with dk-run --project-root <existing directory> --script <this file>.
local function call(method, params)
    local result = dk.command(method, params)
    if not result.ok then
        error(result.error.name .. ": " .. result.error.message)
    end
    return result.value
end

local state = call("scene.new", {name = "Luau 场景", assets = dk.array()})
local function guard()
    return {document_id = state.document_id, revision = state.revision}
end

local parent
for i = 1, 3 do
    local created = call("entity.create", {guard = guard()})
    state = created.state
    local id = created.created_id
    state = call("entity.set_name", {guard = guard(), id = id, name = "实体 " .. i}).state
    state = call("entity.set_transform", {
        guard = guard(), id = id,
        transform = {translation = {i * 2, 0, 0}, rotation = {0, 0, 0, 1}, scale = {1, 1, 1}},
    }).state
    state = call("entity.set_parent", {guard = guard(), id = id, parent = parent or dk.null}).state
    state = call("entity.set_assets", {guard = guard(), id = id, assets = dk.array()}).state
    if i == 1 then parent = id end
end

assert(call("scene.query").state.entity_count == 3)
state = call("scene.save", {guard = guard()})
call("project.save", {guard = guard(), manifest = "project.json"})
