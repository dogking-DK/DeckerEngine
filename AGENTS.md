# DeckerEngine 开发约定

工程名称为 DeckerEngine，C++ 命名空间为 dk。
CMake target 使用 dk_* / dk::*，构建选项和宏使用 DK_*，程序使用 dk-*。

进行实现、修复、重构或构建/依赖调整前，读取并应用
[decker-spec-workflow](.agents/skills/decker-spec-workflow/SKILL.md)。
流程规范在 [spec/README.md](spec/README.md)：先创建或更新模块设计，
再实现；按改动规模留档，重要开发使用编号记录并维护创建/修改时间。
小型修改可合并到相关记录，简单补丁可免写开发日志；具体粒度以 spec 规范为准。
只读分析无需新建开发记录。

从 [模块与源码索引](spec/design/README.md) 定位相关设计、实现和测试；
优先阅读架构边界、相关模块设计和最近关联开发记录，已读取且未变化的内容无需重复加载。
阶段目标、依赖和近期任务见 [开发 Roadmap](spec/roadmap.md)。
三方库用途、版本和接入状态见 [三方库说明](spec/third-party-libraries.md)。
新增或升级依赖默认采用 vcpkg 官方收录的最新版本（含 port 修订），核验后固定 builtin-baseline；
同步该说明与相关设计/验证记录，日常构建不使用浮动版本。
新增或变更命令时同步 [命令参考](spec/commands/README.md)，规则见 spec 规范。
不把预留目录或依赖 feature 当作已实现能力。
公开接口位于 include/dk/，内部实现位于 src/；
模块依赖通过 target 的 PUBLIC/PRIVATE/INTERFACE 表达，不使用全局 include/link。

构建配置见 [构建指南](spec/guides/build.md)，测试入口见 [选择表](.agents/skills/decker-build-verify/references/test-selection.md)。修改后执行与改动相关的验证，
并在开发记录或交付说明中区分通过、失败与未运行的检查。
默认只验证目标功能及直接受影响链路，不固定执行全量或 Debug/Release 双配置。
按任务使用以下 skill，具体操作仍以模块设计和命令文档为准：

- [decker-build-verify](.agents/skills/decker-build-verify/SKILL.md)：选择并执行定向构建/测试。
- [decker-state-contracts](.agents/skills/decker-state-contracts/SKILL.md)：状态变化、提交点和失败保护。
- [decker-command-development](.agents/skills/decker-command-development/SKILL.md)：命令实现、契约和文档同步。

Git 提交使用简明标题和详细正文，说明问题/目标、主要行为和文件范围、
相关 Mx.y/开发记录、验证命令与结果、跳过项和实际限制。不要只写一行标题，
也不要把计划中的能力或未运行的验证写成已完成；每个已验收小阶段单独本地提交。
多行提交正文先写入临时文件，再使用 git commit -F，保留真实换行。
