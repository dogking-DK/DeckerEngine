# DeckerEngine 开发约定

工程名称为 DeckerEngine，C++ 命名空间为 dk。
CMake target 使用 dk_* / dk::*，构建选项和宏使用 DK_*，程序使用 dk-*。

进行实现、修复、重构或构建/依赖调整前，读取并应用
[decker-spec-workflow](.agents/skills/decker-spec-workflow/SKILL.md)。
流程规范在 [spec/README.md](spec/README.md)：先创建或更新模块设计，
再实现；按改动规模留档，重要开发使用编号记录并维护创建/修改时间。
小型修改可合并到相关记录，简单补丁可免写开发日志；具体粒度以 spec 规范为准。
只读分析无需新建开发记录。

优先阅读 [架构总览](spec/design/architecture.md)、相关模块设计和最近开发记录。
阶段目标、依赖和近期任务见 [开发 Roadmap](spec/roadmap.md)。
不把预留目录或依赖 feature 当作已实现能力。
公开接口位于 include/dk/，内部实现位于 src/；
模块依赖通过 target 的 PUBLIC/PRIVATE/INTERFACE 表达，不使用全局 include/link。

构建和验证命令见 [README.md](README.md)。修改后执行与改动相关的验证，
并在开发记录或交付说明中区分通过、失败与未运行的检查。

Git 提交使用简明标题和详细正文，说明问题/目标、主要行为和文件范围、
相关 Mx.y/开发记录、验证命令与结果、跳过项和实际限制。不要只写一行标题，
也不要把计划中的能力或未运行的验证写成已完成；每个已验收小阶段单独本地提交。
多行提交正文先写入临时文件，再使用 git commit -F，保留真实换行。
