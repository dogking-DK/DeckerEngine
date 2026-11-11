# DeckerEngine 开发约定

工程名称为 DeckerEngine，C++ 命名空间为 dk。
CMake target 使用 dk_* / dk::*，构建选项和宏使用 DK_*，程序使用 dk-*。

按任务读取，不把本文件中的链接当作必读清单；本会话已提供且未变化的内容无需重复加载。

- 只读问答/规划：从问题中的路径或模块开始，不启动开发留档流程。
- 实现、修复、重构、构建/依赖调整：读取并应用 [decker-spec-workflow](.agents/skills/decker-spec-workflow/SKILL.md)。
  先创建或更新模块设计，再实现；重要开发使用编号记录并维护创建/修改时间，小改可合并、简单补丁可免日志。
- 纯文档/skill 维护：只读受影响文档及适用规范，不默认加载引擎设计或构建指南。

已知模块直接进入相关设计/源码；未知时用 `rg -n` 在 [模块与源码索引](spec/design/README.md) 定位对应行。
长文档先搜索关键词/标题，再读相关章节；默认不整读 [Roadmap](spec/roadmap.md)、设计索引或开发索引。
仅阶段规划/推进、依赖或当前状态核对需要 Roadmap；“下一步”先定位其“当前执行边界与下一步”，再读目标阶段。
新模块、依赖方向变化或跨模块改动补读架构边界；续做、追查决策或设计/实现冲突时才补读关联历史记录。
读取范围与留档细则以 [spec 规范](spec/README.md#文档职责与读取范围) 为准；信息足以行动即停止扩读，有缺口再补。

三方库用途、版本和接入状态见 [三方库说明](spec/third-party-libraries.md)。
新增或升级依赖默认采用 vcpkg 官方收录的最新版本（含 port 修订），核验后固定 builtin-baseline；
同步该说明与相关设计/验证记录，日常构建不使用浮动版本。
新增或变更命令时同步 [命令参考](spec/commands/README.md)，规则见 spec 规范。
不把预留目录或依赖 feature 当作已实现能力。
公开接口位于 include/dk/，内部实现位于 src/；
模块依赖通过 target 的 PUBLIC/PRIVATE/INTERFACE 表达，不使用全局 include/link。

配置构建时查 [构建指南](spec/guides/build.md) 对应章节；选择测试时搜索 [选择表](.agents/skills/decker-build-verify/references/test-selection.md) 的相关行。修改后执行定向验证，
并在开发记录或交付说明中区分通过、失败与未运行的检查。
默认只验证目标功能及直接受影响链路，不固定执行全量或 Debug/Release 双配置。
自动化启动 dk-editor 时使用 `--background`，避免测试窗口抢占用户焦点；普通交互启动不加此项。
按任务使用以下 skill，具体操作仍以模块设计和命令文档为准：

- [decker-build-verify](.agents/skills/decker-build-verify/SKILL.md)：选择并执行定向构建/测试。
- [decker-state-contracts](.agents/skills/decker-state-contracts/SKILL.md)：状态变化、提交点和失败保护。
- [decker-command-development](.agents/skills/decker-command-development/SKILL.md)：命令实现、契约和文档同步。

Git 提交使用简明标题和详细正文，说明问题/目标、主要行为和文件范围、
相关 Mx.y/开发记录、验证命令与结果、跳过项和实际限制。不要只写一行标题，
也不要把计划中的能力或未运行的验证写成已完成；每个已验收小阶段单独本地提交。
多行提交正文先写入临时文件，再使用 git commit -F，保留真实换行。
