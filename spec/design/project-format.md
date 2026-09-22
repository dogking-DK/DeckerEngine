---
module: project-format
created_at: "2026-09-22T12:28:05+08:00"
updated_at: "2026-09-22T13:06:18+08:00"
status: accepted
---

# 工程格式与资源解析（M2.3）

Project.hpp 位于 engine/scene，链接 dk::scene。该 target 从本节起 PUBLIC 依赖
dk::io 与 dk::asset_types，PRIVATE 依赖 nlohmann-json 3.12.0；scene vcpkg feature
增加 nlohmann-json，关闭日志不引入 fmt/spdlog。Scene 要求 math/io 同时开启。

## v1 协议

UTF-8 JSON 顶层字段严格为 format="DeckerProject"、version=1（整数）、name、
scene（场景相对路径）、assets（资产记录数组）。每条记录严格包含 id、kind、path。
版本未知为 not_supported，结构/类型/未知字段/重复对象键/重复或 nil ID 为 invalid_argument。
name 非空、至多 1024 字节、无 NUL、合法 UTF-8；资产最多 10000 条。
文件输入上限 16 MiB，JSON 嵌套深度上限 64；不接受注释或尾随内容。
路径要求合法 UTF-8、1–4096 字节，使用 /、相对且已规范化；
不允许绝对路径、反斜杠、冒号、空路径段、. 或 .. 路径段。子路径符号链接沿用 IO 的词法边界。

parse_project/serialize_project 在内存中转换 ProjectDescription 值，
输出按 AssetId 排序，便于 diff；不暴露 nlohmann 类型。serialize 也执行完整字段验证。
Project::create(root, description) 固定已有工程根并建立只读资产索引，描述返回 const 引用。
Project::open(root, relative_manifest) 限长读取后创建；工程根由调用者明确指定，
资产和 scene 均相对这个根，不受清单所在子目录或之后 cwd 影响。

resolve_asset(reference) 检查非 nil/合法种类、注册 ID、类型匹配、普通文件存在性；
未登记/缺失为 not_found，种类不匹配/目录为 invalid_argument，系统错误为 io_error。
错误带 AssetId/相对路径上下文。scene_path 返回解析后的目标，不要求已存在，
以允许首次保存。validate_files 检查场景和全部已登记资产，返回首个错误。
check_asset_references(scene, project) 检查所有实体引用，并补实体 ID 上下文。
create/parse 不要求磁盘资产已存在，便于编辑尚未完成的工程；使用与验收时显式校验。

## 验证和边界

M2.4 新增 save_project(project, relative_manifest)：编码后使用带验证器的 atomic writer，
临时文件读回比对并重新 parse_project 成功后才替换目标；工程根固定且父目录须存在。
Scene 文件持久化由 [Scene M2.4](scene.md) 规定；此接口不修改资产或场景文件。

验证 JSON 往返/确定输出、版本与类型、重复键/ID、nil、路径越界、超限/深度、
非法 UTF-8、引用错误保留场景 revision、真实 Unicode 工程根/文件以及缺失诊断。
默认 Debug/Release 回归，最小 Scene 配置新增 IO/JSON；无日志或 GPU 依赖。
文件写入在 M2.4 通过 Foundation atomic writer 提供；M2.3 只返回编码文本和读取。
不提供 schema 迁移、资产解码或持续文件监视。

参考：[nlohmann parse](https://json.nlohmann.me/api/basic_json/parse/)、
[parser callback](https://json.nlohmann.me/api/basic_json/parser_callback_t/)、
[IO](foundation-io.md)、[资产类型](assets-types.md)、[0011](../development/0011-project-assets.md)。
