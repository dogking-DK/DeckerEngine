---
module: render-capture
created_at: "2026-10-03T21:21:47+08:00"
updated_at: "2026-10-08T13:57:00+08:00"
status: accepted
---

# M7.4 截图作业与自动化验收

## 边界与依赖

新增可选 `DK_BUILD_RENDER_CAPTURE`，默认 OFF，windows-graphics 启用。
`dk_render_services` 依赖 SceneServices、Jobs、RenderDisk、RenderPipeline；
`dk_render_operations` 负责注册和 JSON 转换；Runtime 条件装配。
CPU-only Runtime 保持不依赖 Vulkan/Slang。复用 [Jobs](foundation-jobs.md) 和
[命令协议](automation-protocol.md)，不改同步 TaskId envelope，不新增传输分支。
具体参数见 [截图命令](../commands/render.md)。Jobs 的 JSON 注册拆到独立 JobOperations，
AssetOperations 默认仍注册资产 jobs；启用截图的 Runtime 改为一次注册统一路由。
当前 schema 子集无 oneOf，统一结果采用两种对象字段的可选并集，服务保证完整变体；CPU-only 保留资产严格 schema。

## 输入、快照与身份

`SceneService::read_snapshot(guard)` 在 owner 线程复制 Project 映射和 SceneSnapshot，
保留 document_id、scene_id、revision；worker 不访问 ECS 或 SceneService。
`DiskScene::load_snapshot` 使用该快照解析资产，不重新加载磁盘 Scene，因此未保存编辑可截图。
文件资产在 worker 执行时读取，不承诺外部文件热修改的时间点快照。
每次接受 capture 分配递增非零 frame（Runtime 会话内唯一，拒绝不消耗）；返回 JobId 和固定
文档/场景/revision/frame/尺寸/相对输出路径。后续编辑、撤销、换场景不会改变该输入。
终态 succeeded 的 result 返回同一身份及 draw_count；失败/取消 result 为 null，提交响应保留关联。

## 执行、提交点与关闭

CaptureService 独立拥有 MemorySystem、render/jobs heap、owner ThreadContext 与单 worker JobQueue。
queued=2、active=3、terminal=64、输入预约上限 192 MiB；每份 SceneSnapshot 上限 32 MiB，
宽高各 1..2048。预约包含快照、Project 路径映射、请求和最大读回/PPM字节；
CPU 资产仍受 DiskScene 的 64 个/512 MiB 载荷限额，只有一个 worker 同时导入/使用 GPU。

worker 依次创建候选 CPU 资产、独占 Device/SubmissionQueue、上传并等待、建管线、绘制、
等待 GPU、读取 RGBA8、编码 P6 PPM；GPU 对象在 worker 内回收，completion 只含拥有型字节与摘要。
不跨 worker 共享队列。首次捕获才初始化设备；创建 Runtime/查询能力不要求 GPU 存在。
取消在阶段间及 glTF importer 检查；已提交 GPU 工作必须等待并回收，不能抢占驱动/三方 IO。

owner drain 在非重入安全点通过 `write_file_bytes_atomic` 发布单个文件，原子替换是提交点。
取消先被 owner 接受则 consumer 不运行，旧文件保持；发布后取消 accepted=false。
IO/导入/GPU 可恢复错误进入 failed，旧文件保持。summary 在文件提交前完成必要分配，
基础设施异常仍沿 Jobs fatal 路径退出，不承诺进程中断/OOM 的跨资源事务。
stdout 只输出 JSON-RPC；诊断由 Device stderr sink 处理。验证开启时检查 validation errors。
默认设备 validation=if_available，集成验收要求 required。

Runtime idle pump 同时消费资产与截图；`jobs.*` 通过 JobId 路由到所属队列，
查询未知/被淘汰 ID 为 not_found。jobs.wait 最多1000ms、泵两种完成但不重入命令。
关闭取消并 join 后释放 completion/context/heap，不隐式保存场景、也不发布未消费截图。

## 限制与验证

只输出 PPM（顶部第一行、sRGB RGB8），父目录必须存在；路径在工程根内，输出扩展名 .ppm。
相机使用 eye/target/up、透视 fov_y/near/far，默认采用 Sponza 示例相机；
默认 unlit_preview，支持 opaque/mask，不新增 PBR/透明/天空盒/HDR 照明。
M8.4 与编辑器共用 Render 的 unlit_preview_settings。比较视口和截图必须显式匹配尺寸与相机，
包括 FOV/near/far（编辑器 far=10000，截图默认 far=100），并固定资产文件与设备；
比较对象是已提交场景，草稿/手势候选不在 capture 快照内。完整验收见 [编辑器设计](editor.md)。
重复输出按实际 owner 发布顺序替换，无多文件事务；成功只说明该次写入，后续 capture 可覆盖。

CPU 命令验收覆盖 discovery/schema、guard、无场景、路径/相机/尺寸、事务拒绝；
GPU 进程验收覆盖版本绑定、编辑后图像变化、无输入自动发布、失败保留、取消和退出。
原资产 jobs 定向回归与 CPU-only runner 构建确认原有能力和依赖边界。
