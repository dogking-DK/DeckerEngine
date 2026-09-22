# DeckerEngine 开发约定

工程名称为 DeckerEngine，C++ 命名空间为 dk。
CMake target 使用 dk_* / dk::*，构建选项和宏使用 DK_*，程序使用 dk-*。

进行实现、修复、重构或构建/依赖调整前，读取并应用
[decker-spec-workflow](.agents/skills/decker-spec-workflow/SKILL.md)。
流程规范在 [spec/README.md](spec/README.md)：先创建或更新模块设计，
再实现；开发事实写入编号开发记录，维护创建时间和最后修改时间。
只读分析无需新建开发记录。

优先阅读 [架构总览](spec/design/architecture.md)、相关模块设计和最近开发记录。
不把预留目录或依赖 feature 当作已实现能力。
公开接口位于 include/dk/，内部实现位于 src/；
模块依赖通过 target 的 PUBLIC/PRIVATE/INTERFACE 表达，不使用全局 include/link。

构建和验证命令见 [README.md](README.md)。修改后执行与改动相关的验证，
并在开发记录中区分通过、失败与未运行的检查。

