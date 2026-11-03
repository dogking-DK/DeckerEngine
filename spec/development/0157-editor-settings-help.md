---
id: "0157"
created_at: "2026-11-03T17:23:00+08:00"
updated_at: "2026-11-03T17:57:12+08:00"
status: completed
design_refs:
  - ../design/editor-console.md
  - ../design/editor.md
  - ../design/commands.md
---

# 0157 M22.10 Editor 字段帮助与设置搜索

## 目标与设计依据

完成 M22.10；基线 4b9a954，初始工作区干净。先明确共享元数据、稳定定位、可用原因和模板输入保护，再实现。
范围及状态边界见 [实施契约](../design/editor-console.md#m2210-实施契约)。M22.11 集成收尾不在本阶段。

## 实际变更

- `SettingsCatalog` 以稳定 ID 绑定现有面板与真实命令 schema/help；与 Console 共用 `CommandFieldHelp`，补齐数组分量类型/范围显示。
  实体名称/TRS 帮助说明米制平移、XYZW 四元数、缩放比例；simulation.run 说明原有参数默认值和单位。命令数和业务校验未改变。
- 工具栏 Settings 搜索支持中文别名、规范参数和多词 AND；256 UTF-8 字节输入、64 项稳定结果。详情及悬停说明类型/范围/单位、已定义默认值、持久化/Undo/采样影响和不可用原因。
  覆盖 Console、Inspector、Environment 物理/本地预览、Edit 相机、Path tracing、Simulation 下一实验/结果导出和控制。
- Locate 以语义 ID 聚焦面板、展开栏目/相机弹层或 Simulation tab、滚动并高亮约 3 秒；不选实体、不切换模式、不 Apply。
  ReSTIR/Scene media 从原条件隐藏改为禁用显示；无选中实体、缺命令/能力、独立 Simulation 视图有说明入口。
- 只为实际接口生成命令模板，递归保留全部必填参数/对象及目标字段，占位不含当前 guard/UUID。Copy 明确复制；Insert 仅写空 Console 且无 IME 组合输入。
  保留既有输入/草稿，不执行、不记历史；无接口的本地云预览设置不造模板。
- 新增 6 项 CPU 用例和独立 `--settings-help-smoke` / `dk.editor.settings_help_gpu_validation`；原生 ImGui 输入共 69 步，后台截图和文件哈希核对。
  同步模块设计、索引、命令参考、Editor 指南和验证选择表。

## 验证记录

配置 windows-editor / Debug；按每次范围构建 dk_editor_tests、dk_editor_app，以及回归需要的 dk_run/dk_ctl，单构建目录串行执行。
GUI 均使用 --background，required Vulkan/synchronization validation；进程设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，
只屏蔽既有 AMD 可选切换层警告，不关闭验证或降低业务断言。

| 验证目录 | 实际结果 |
| --- | --- |
| out/verify/20261103-173910-3e0433b8 | 首轮编译失败：当前 ImGui AddRect 有兼容重载，整数实参歧义；改为当前签名的明确 float/flag 后通过，测试未运行 |
| out/verify/20261103-174200-2c907aca | 11 项 CPU 通过；新只读性测试把 Console 查询自身刷新产生的 4 个任务误当为 1 个，断言失败；GUI 驱动尚未完整接入，终止本次后台验收进程，不能算通过 |
| out/verify/20261103-174347-31693dd5 | 新 6 项 CPU 全通过；Workbench 在上轮编译时修改，旧对象时间戳晚于源文件，增量构建未重编译，GUI 驱动未执行；终止本次进程并刷新 Workbench/main 源时间强制编译，未清缓存/改配置 |
| out/verify/20261103-174637-60aa6831 | 5 项 Console assistance、5 项设置 CPU 和新 GUI 通过；新增数组范围断言误以为 environment.sun.irradiance 的形状 schema 带物理上界，修正为实际带上界的 path.environment_rgb；环境物理约束按 Scene 校验另行说明 |
| out/verify/20261103-174919-07ac55ba | 6 项设置 CPU + 6 项后台 GUI 全通过：Settings 17.60s、原 Workbench/Console 24.43s、交互 14.59s、视口命令 7.46s、相机命令 8.48s、ReSTIR 39.62s |
| out/verify/20261103-175257-e68b9a3a | 缺能力/独立视图说明保留与 dirty 检查补充后，6 项设置 CPU 与 Settings GUI 通过；GUI 17.68s |
| out/verify/20261103-175524-8a3ab05a | 相机弹层未 Apply 的自动裁剪草稿驱动 Near/Far 提示，追加点击/悬停/Cancel 检查；最终 Settings GUI 69 步通过，18.50s |

只读任务账本用相邻控制查询比较新增任务数量和方法序列，排除查询自身的刷新成本；测试不把它当作帮助的业务 Task。
原生 GUI 同时检查 Scene revision、Undo、相机/渲染 guard 与设置、无模拟启动、无 Console 历史/错误增长；显式创建的 Inspector 中文草稿和已插入输入保持不变。
夹具 scene.json SHA-256 不变；新 GUI 正常关闭的 errors=0、warnings=0、liveAllocations=0。
初次截图发现底部两行插入提示裁切，改为预留四行底部空间并设置最小窗口尺寸，最终截图复核：中文显示完整、底部插入限制提示可见，详情独立滚动。辅助 PNG：out/m2210-settings-help.png。

实际 runner 的三条 commands.describe（simulation.run、entity.set_transform、entity.set_name）返回 help v1，时间步默认值与平移米制单位核对通过；证据 out/m2210-describe-results.jsonl。

最终累计 12 项不同 CPU 检查（6 设置、5 Console assistance、1 catalog）及 6 项不同后台 GUI 全部通过。
未把前述失败/中止轮次算作通过；本阶段通过的最新证据覆盖所有本次修改。
check-spec 对 13 个变更 Markdown、943 个本地链接、元数据/表格/索引/测试入口及 JSON manifests 检查通过；git diff --check 通过。

## 偏差与决策

无新增第三方依赖。搜索不变更模式/业务设置，无接口的字段只定位与解释。
搜索匹配不做拼音/模糊推测；相机弹层遵循现有关闭/Cancel 草稿语义，定位已打开弹层不重新读取并覆盖草稿。
这里只复用现有设置，不新增设置存储、自动 guard、位置参数语法或 Simulation 相机命令。

## 遗留问题与下一步

下一阶段 M22.11 集成闭环与交付；本次不执行全量、Release 或其他平台/裁剪模块配置。
缺命令/能力通过 CPU 模型分支检查；实际窗口使用本机完整 Editor 配置，不把它当作所有设备/构建组合的通过证据。
既有 Simulation GPU 频率失败见 [0156](0156-editor-camera-commands.md)，本阶段没有修改发布链路或重跑该性能断言，也不将旧失败当作通过。

## 修改记录

- 2026-11-03T17:23:00+08:00：补齐实施设计并建立记录。

- 2026-11-03T17:57:12+08:00：完成 M22.10 与定向验收；相机弹层使用未 Apply 的自动裁剪草稿生成禁用说明，模式/状态保持原有提交契约。
