---
module: project-foundation
created_at: "2026-09-22T09:09:41+08:00"
updated_at: "2026-10-08T14:42:00+08:00"
status: accepted
---

# 工程基础设计

## 目标与范围

在空目录初始化 main 分支 Git 仓库，建立可扩展模块目录、可重复构建入口、
vcpkg 清单和技术留档流程。本设计记录工程基础，Core 能力增量见
[Core 设计](foundation-core.md)。工程提供 core 版本查询和 dk-run 构建探针，
以验证 C++23 静态库到可执行程序的真实链接；M3.4 扩展为 CPU 场景批处理入口。

## 构建和目录

根目录依次组织 engine、apps、tools、examples、tests。
engine/foundation/core 是首个真实静态库 `dk_core`，别名 `dk::core`。
公开头 `dk/core/Version.hpp` 提供
`[[nodiscard]] std::string_view dk::version() noexcept`；
返回编译生成的项目版本字符串，生命周期为静态期，无分配、线程状态或失败路径。

`dk-run --version` 输出 DeckerEngine 和版本；
无参数或 `--help` 输出当前骨架用法；不支持的参数在 stderr 报错并返回 2。
该程序始终链接 dk::core；FRAMEWORK 与 Scene 开启时链接 automation_server → runtime/protocol/transport。
协议/传输与 Runtime 适配拆分，独立 windows-client 预设只构建 dk-ctl 与 Core/Commands 及轻量自动化层。
M3.4 runner 诊断直接写 stderr，不依赖可选 logging；最小 bootstrap 仍保留原功能范围。
dk-assetc 已提供 CPU 离线资产导入；其他应用和规划模块的当前接入范围见[模块索引](README.md)。

DK_BUILD_EXAMPLES 默认 ON；当前 math/io 同时启用时建立独立 dk-foundation-demo，
不满足依赖时跳过，不反向开启模块。bootstrap 关闭示例。
示例跨进程 CTest 不依赖 runner 或 Catch2，详见 [集成设计](foundation-integration.md)。
M2 开发预设启用 DK_BUILD_SCENE（选项自身默认 OFF），构建 dk::asset_types/dk::scene。
Scene 要求 math/io 同时开启；启用示例时增加 dk-scene-demo 及独立进程验收。
bootstrap 和独立 Foundation 配置显式关闭 Scene；[使用指南](../../README.md#按任务阅读)保留可复现命令。
M3.1 新增 DK_BUILD_FRAMEWORK（默认 OFF，windows-dev ON），构建独立 dk::commands；
commands feature 仅引入 JSON，场景关闭时不反向启用 Scene/Math/IO。
M3.4 Scene 开启时继续装配 services/operations/runtime 与 automation/protocol/transport。
bootstrap 和独立 Foundation/Scene 验证应显式关闭 FRAMEWORK，避免继承开发预设。
M1.7.1 增加真实 dk::profiling 包装：OFF 为无 Tracy 依赖的 INTERFACE target，ON 为静态适配库。
Runtime/IO/runner 按实现需要 PRIVATE 链接，构建开关及采集规则见 [Profiling 设计](foundation-profiling.md)。
M1.7.2 增加 `dk::memory` 静态包装，PRIVATE 链接 mimalloc v3 和 profiling；公开头无三方类型。
`DK_BUILD_MEMORY` 默认 OFF，windows-dev/profiling 启用，bootstrap 保持最小依赖。
`DK_PROFILE_MEMORY` 默认 ON，仅在 `DK_ENABLE_PROFILING=ON` 时发 backing 事件；CPU-only 对照可独立关闭它。

CMake 最低 3.28，C++23 target 使用要求向消费者传播，禁用编译器扩展。
项目警告函数只作用于自身 target，不污染依赖。默认构建目录为 `out/build/<preset>`，
bootstrap 的迁移目录见下文；按配置放置 bin/lib。禁止源码内构建。
CTest 保留 runner 冒烟验证，开发预设额外运行 Catch2 行为测试。

## Visual Studio 解决方案分组

解决方案通过 CMake 的 FOLDER 元数据分类，根目录显式开启 USE_FOLDERS，
将 CMake 自动生成的 ALL_BUILD、ZERO_CHECK、RUN_TESTS 等项目归入 CMake。
各分组目录在 add_subdirectory 前设置 CMAKE_FOLDER，由下级真实 target 继承：

- engine → Engine；foundation、assets、automation、framework 分别追加
  Foundation、Assets、Automation、Framework 子组；dk_scene 直接位于 Engine。
- apps → Apps；当前包含 dk_run。
- tests → Tests；包含各单元测试和 dk_log_probe，CTest 的脚本测试继续由 RUN_TESTS 执行。
- examples → Examples；包含 Foundation 和 Scene 示例。
- tools → Tools；当前没有真实 target，不生成空的解决方案文件夹。

后续模块在对应目录创建 target 时自动继承分组；需要进一步细分时，
在该目录以 `${CMAKE_FOLDER}/子组名` 扩展。只控制 IDE 展示，不改变 target 名称、
编译选项、链接关系、默认构建集合或测试注册，不手改生成的 slnx/vcxproj 或 VS 个人配置。
通过根脚本重新生成后，已打开的 VS 接受重新加载提示即可更新树形结构。
验证生成前后的项目/依赖/配置保持一致、所有项目均归组，并检查完整与最小预设。
本次为工程基础维护，合并记录到 [0018](../development/0018-vs2026-generation-script.md)。

## CMake Presets

- `windows-bootstrap`：Visual Studio 18 2026 / x64，vcpkg 只安装必需的 stduuid，
  关闭日志/数学/IO/单元测试，构建目录为 out/build/windows-bootstrap-stduuid；
  此生成器要求 CMake 4.2 或更新。
- `windows-dev`：相同生成器，开启 vcpkg 并选择 foundation feature。
- `windows-profiling`：继承 windows-dev，启用 DK_ENABLE_PROFILING，build/test 使用 RelWithDebInfo；
  环境限制 localhost/IPv4，手动启动程序时须自行传入同样环境。
- `windows-desktop-deps`：准备后续桌面功能依赖；安装依赖不表示模块已实现。
- `ninja-debug / ninja-release`：Ninja 构建入口，需调用者提供匹配的编译器环境。

共享 presets 不包含本机绝对路径。个人覆盖用已忽略的
`CMakeUserPresets.json`，vcpkg 根路径通过 `VCPKG_ROOT` 环境变量提供。

## VS2026 工程生成入口

根目录提供 generate-vs2026.bat，使用 Windows 自带 cmd，复用 windows-dev configure preset，
不另存一份生成器/架构/依赖选项。以脚本所在目录为工作目录，执行后恢复调用者目录；
配置阶段按现有 vcpkg manifest 准备依赖，输出包含 Debug/Release 的 VS2026 x64 解决方案。
检查 PATH 中的 CMake，失败时给出 CMake 4.2+ 安装提示；配置失败原样返回非零退出码。
成功显示 out/build/windows-dev/DeckerEngine.slnx；兼容生成 .sln 时显示对应路径。
没有自动清理缓存、编译或启动 IDE。必要条件继续是 VS2026 C++ 工具、CMake 和已配置的 vcpkg。
验证从其他工作目录实际调用、生成的解决方案和生成器/架构，以及缺少 CMake 时的诊断。
这是工程基础维护，不推进 M4；记录见 [0018](../development/0018-vs2026-generation-script.md)。

## vcpkg 策略

使用 manifest mode，固定 builtin-baseline 为
`33d78c1ed898a06938f31312167c7abefd229455`（2026-09-23 M1.7.1 查询的官方 master）。
默认采用 vcpkg 官方收录的最新 port 版本，包括 port 修订；当前清单与维护规则集中见
[三方库说明](../third-party-libraries.md)。默认不添加旧版本 override，也不在每次配置时跟随浮动 master。
升级时先抓取官方索引，核对所有直接依赖、相关传递依赖及规划选型，再固定提交。
本机 vcpkg 干净 checkout 只做 fast-forward，同步其 bootstrap 要求的工具版本。
依赖 feature 按模块选择，包含 foundation、math、memory、scene、commands、graphics、vulkan-device、shaders、editor、scripting、tests、profiling 等，
清单默认安装 Core 必需的 stduuid，可选库仍按 feature 选择。
`scripting` 预留 feature 改为仅选择 Luau，不再选择 Lua/sol2，尚无引擎 CMake 消费者。
为只更新此次选型，Luau 在 [vcpkg-configuration.json](../../vcpkg-configuration.json) 中
使用官方 Git registry，reference/baseline 均固定为 `2750401336fb7c95f6619657a46a7e798661341c`，
仅匹配 `luau`，版本 0.741（port #0）。其余包继续由上述 builtin-baseline 解析，避免连带升级已验收模块。
这是此次单包替换的固定 registry 例外，不引入旧版本 override、浮动 reference 或自制 port。
后续统一升级 builtin-baseline 时，若已包含所需 Luau 版本，应一并移除这项单包 registry。
M9.1 接入时重新核验版本，计划 PRIVATE 链接官方 Compiler/VM 导出；源码目录为 engine/scripting/luau。
stduuid 在 Core 实现中使用，不暴露到公开头。
数学模块使用独立 math feature 安装 eigen3，启用 DK_BUILD_MATH 时自动补充该组；
foundation 中移除未使用的 glm，保留日志及 JSON 依赖。M2 的 scene feature
也声明 flecs/nlohmann-json，关闭日志的场景配置不需要 fmt/spdlog。
dk::math 的公开 Eigen 类型要求 PUBLIC 传递 Eigen3::Eigen，详见
[数学设计](foundation-math.md)。bootstrap 显式关闭数学，保持最小依赖构建。
IO 通过默认开启的 DK_BUILD_IO 构建 dk::io，PUBLIC 链接 dk::core，PRIVATE 链接可关闭的 dk::profiling；
bootstrap 也关闭 IO，接口和验证见 [IO 设计](foundation-io.md)。
profiling feature 选择 Tracy on-demand 且关闭默认 features；开启 DK_ENABLE_PROFILING 时自动补充。
memory feature 选择 mimalloc 3.5.3 且关闭默认 features；开启 DK_BUILD_MEMORY 时自动补充，不启用 override。
Tracy 同版本 overlay 补充客户端 TRACY_ENABLE=ON；mimalloc 同版本 overlay 关闭 MI_WIN_REDIRECT，
不通过消费方宏或过滤 stderr 掩盖依赖配置。具体状态见[三方库说明](../third-party-libraries.md)。
工具端清单独立位于 tools/profiling，不在引擎构建中启用 GUI/CLI 工具 feature。

`DK_VCPKG_FEATURES` 在首次 `project()` 前映射到
`VCPKG_MANIFEST_FEATURES`，并验证 feature 名。关闭 vcpkg 时不能选择 feature；
已注入 vcpkg 的构建目录不能切换到关闭状态，应使用另一构建目录。
具体模块落地时在其 CMakeLists 中添加 find_package 和 target_link_libraries，
不能将整个依赖集合全局链接到每个模块。

Windows 使用动态库/动态 CRT 的 x64-windows triplet，与 Slang 预编译库适配。
Slang 包名是 `shader-slang`，CMake package 是 `slang`。独立 shaders feature 供
DK_BUILD_GRAPHICS_SHADERS 使用，不引入 Vulkan；graphics 保留聚合依赖。
dk-shaderc 使用当前平台的 Slang 库，标准模块随可执行文件部署；当前未承诺交叉编译 host 工具管理。

项目依赖缓存留在构建目录的 vcpkg_installed，忽略 build/cache/log 和 IDE 个人文件。
更新 baseline 必须连同依赖变化、构建验证和编号开发记录一起提交。
此前升级及版本表见 [0021](../development/0021-vcpkg-baseline-update.md)。当时实际安装 windows-dev
已启用依赖，全部现有 feature 另作依赖解析检查；M4 选型同步新基线，接入仍在对应实施小节完成。
验证按更新影响选择：fmt/spdlog 日志、flecs 场景/服务，以及 Catch2 使用者的构建和代表性用例；
默认只使用 Debug，不因升级工具或索引而自动运行全部配置。
M1.7.1 接入与新基线证据见 [0023](../development/0023-tracy-cpu-profiling.md)；既有库版本未变，
仅对 profiling 条件编译及直接受影响链路做 OFF/ON 验证，不重复全依赖回归。
M1.7.2 再次核验同一官方基线，新增 heap/关闭闸门/预算及内存事件定向验证，见
[0024](../development/0024-mimalloc-heap.md)。内存读取工具另建于 tools/profiling/inspector，
不加入引擎解决方案；bootstrap 配置确认不自动装配 mimalloc/Tracy。
协议进程测试按 runner 的 UTF-8 契约显式解码 stdout/stderr，不依赖 Windows 活动代码页
或独立 CMake 脚本的默认 policy；往返用例同时核对预期中文名称，避免相同乱码被误判为一致。

## 留档与 skill

流程规范由 [spec/README.md](../README.md) 维护，模板置于 spec/templates。
仓库 skill 路径为 `.agents/skills/decker-spec-workflow/SKILL.md`，
根 AGENTS.md 让后续任务先读取该 skill。
本模块设计先落盘，再实现构建文件；开发事实持续写入
[0001](../development/0001-project-bootstrap.md)。

## 开发辅助 skills

在 `.agents/skills` 增加三个按任务选择的仓库 skill，入口说明触发范围与工作方法，
不复制模块设计或全部命令；保留既有 decker-spec-workflow 的留档粒度规则。

- decker-build-verify：根据改动及直接受影响调用链选择构建目标/测试，默认单配置、局部验证。
  scripts/verify.ps1 接收 BuildDir、Configuration（默认 Debug）、Target 和 TestRegex；
  显式 Full+Reason 才构建/测试全部，不自行切换配置或扩展范围。
  要求已配置构建目录；先构建指定目标，失败停止；再枚举匹配测试，零匹配报错，最后执行 CTest。
  每次独立保存日志、JUnit 和 summary.json 到 out/verify，记录提交/工作区、所选测试及通过/失败/跳过。
  调用者须确认 Target 覆盖所选测试程序及进程夹具；脚本不从文件名猜测依赖，也不自动配置或清理缓存。
- decker-state-contracts：明确修改状态、成功结果、失败保护、提交点和必要案例；
  仅将内存编辑/事务、持久化等已实践模式放入按需参考，具体操作仍归属模块设计。
- decker-command-development：复用 Commands/Operations/Services 和现有传输，
  同步 spec/commands 的目录、字段、guard、副作用和示例，选择受影响命令测试。

scripts/check-spec.ps1 将现有临时文档检查提升为仓库脚本，检查 README/AGENTS/spec/skills
中的本地链接、spec 元数据、开发编号及 JSON 清单；支持限定文件检查。
这三个 skill 不新增常驻 agent、并行委派或逐次小改留档要求。
验证采用辅助脚本的正常/失败/零匹配/跳过案例、真实局部测试及 skill 格式/链接检查。
本组改动合并记录到 [0019](../development/0019-development-skills.md)，不推进 M4。

## 文档入口与一致性检查

维护记录：[0041](../development/0041-ai-documentation-workflow.md)。根 AGENTS 保留稳定约束和路由，
spec/README 是流程规范；README 保留快速开始和导航，长命令与使用示例按主题置于 spec/guides。
Roadmap 维护阶段状态和依赖，模块设计描述当前契约，编号记录保存历史决策与当次验证证据。
设计索引补充源码/测试入口；最近记录通过其 design_refs 定位，不维护另一份“最新编号”表。
既有四个 skill 按需读取；状态参考补充异步发布、关闭回收和缓存恢复方法，不复制具体 API。
协作约定放在 spec 流程中，仅在已授权使用多 agent 时适用，不自动启动并行任务。

check-spec 保持只读、无需配置引擎或安装 Markdown/YAML 依赖：

- 默认扫描 README、AGENTS、spec、skills，限定 Path 时只扫描指定 Markdown；全局索引、编号与测试入口检查仍执行。
- 在本仓库的行式元数据约定下检查 module/id/status、时间与 design_refs；校验设计/开发索引与文件一一对应、状态一致。
- 忽略 fenced code 后检查本地文件链接和断开的表格行；不把代码示例当成文档链接或表格。
- 对测试选择表的 target 检查源码中的字面 add_executable 注册，并检查 tests 下注册的测试程序都有入口。
  这只是静态入口覆盖，不解析 CMake 条件或推断 regex 完整性；实际可用性、名称和命中数仍由 verify/CTest 核对。
- 不验证远端页面、Markdown 锚点或自然语言语义，不以脚本通过代替设计审阅。

脚本验证采用临时独立仓库夹具，覆盖正常文档、索引遗漏/状态失配、错误引用、断表、未登记测试入口等行为；
PowerShell 7 和 Windows PowerShell 5.1 各运行一次。文档移动核对命令块保留与链接更新，
不改变 C++、构建选项或可执行示例行为，因此不运行引擎回归。

## 验证计划与取舍

当前工程在初始化骨架上按 [Core 设计](foundation-core.md) 扩展：
dk::core 增加错误和 ID，dk::logging 为可选日志 target。
DK_BUILD_LOGGING、DK_BUILD_MATH、DK_BUILD_IO 和 DK_BUILD_UNIT_TESTS 默认开启；
vcpkg 自动补充所需依赖组。windows-bootstrap 显式关闭四者，保留仅依赖 stduuid 的最小探针；
开发预设使用 Catch2 增加真实行为测试，并实际链接日志依赖。
以下原始骨架验证仍保留，Core 增量验证见 [0002](../development/0002-foundation-core.md)，
stduuid 迁移及最小预设调整见 [0003](../development/0003-stduuid-migration.md)。
Eigen 数学、math feature 与阶段细分见 [0004](../development/0004-eigen-math-foundation.md)。
IO 开关和独立 Core/IO 验证见 [0006](../development/0006-foundation-io.md)。

运行 configure/build/CTest，检查版本与错误参数行为；检查预设 JSON、
vcpkg feature 解析、skill 格式、文档链接及 Git 忽略项。
真实安装受网络、编译器、缓存影响，未完成的依赖构建应如实记录；
最小依赖构建与完整桌面依赖验证分别记录。

暂不建立大量空静态库、不实现 Vulkan 初始化；Catch2 已用于当前 Core 单元测试。
不定义安装导出/打包协议；模块 API 稳定后再设计。
未进行完整读取的原会话后半部分不作为本次已确认事实。

## 参考

- [CMake Presets](https://cmake.org/cmake/help/v4.2/manual/cmake-presets.7.html)
- [vcpkg CMake integration](https://learn.microsoft.com/en-us/vcpkg/users/buildsystems/cmake-integration)
- [OpenAI 官方 skill 文档](https://learn.chatgpt.com/docs/build-skills)
