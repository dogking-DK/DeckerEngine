---
id: "0018"
created_at: "2026-09-22T15:58:55+08:00"
updated_at: "2026-09-22T16:37:39+08:00"
status: completed
design_refs:
  - ../design/project-foundation.md
---

# 0018 VS2026 工程生成与解决方案分组

用户要求在根目录添加生成 VS2026 工程的脚本；基线 cd8814e，工作区初始干净。
本次为工程基础维护，M3 已完成，M4 尚未开始。
先更新 [工程设计](../design/project-foundation.md)，再实现脚本。

## 实际变更

新增根目录 [generate-vs2026.bat](../../generate-vs2026.bat)，检查 CMake、通过 pushd/popd
以仓库根为工作目录执行 windows-dev，保存并返回配置退出码；成功输出 .slnx（兼容 .sln）路径。
README 增加使用方式和生成位置；.gitattributes 固定 batch 为 CRLF。
生成阶段沿用既有 vcpkg feature 和缓存，不引入新的生成器/依赖配置来源。

## 验证记录

- 从 out 工作目录调用根目录脚本：CMake 4.2.1 配置/生成成功，退出 0；
  现有 vcpkg 依赖全部命中，输出 out/build/windows-dev/DeckerEngine.slnx。
- 解析生成的 .slnx，并检查 CMakeCache：Visual Studio 18 2026、x64，
  Debug/Release/MinSizeRel/RelWithDebInfo 多配置存在。
- 子进程 PATH 仅含 System32：缺少 CMake 的提示明确，返回 1。
- 将脚本复制到 out 下含空格的临时目录，真实 CMake 因缺少 preset 失败：
  退出 1 被原样传播，不输出生成成功提示；未修改共享构建配置。
- 文档链接/元数据/编号/JSON 清单检查和 git diff --check 通过。
- 本次未改 C++/依赖或构建选项，未重复执行引擎编译和完整 CTest，也未启动 VS IDE。

## 后续小改：解决方案分组

基线 4335615。按用户允许小型修改合并记录的新约定，本次继续使用 0018，
保留脚本开发的原始验证记录。先补充工程设计，再实现 Engine/Tests/Apps/Examples/CMake 分组。
spec 流程和仓库 skill 同步“小改合并、简单补丁可免写开发日志”的粒度规则。

根 CMake 显式启用 USE_FOLDERS，设置 PREDEFINED_TARGETS_FOLDER=CMake；
engine/apps/tests/examples/tools 分组目录设置 CMAKE_FOLDER，Engine 下按 Foundation、
Assets、Automation、Framework 细分，子目录 target 继承。README 补充 VS 重新加载方式。

本次验证：

- 根脚本和 `cmake --preset windows-bootstrap` 均重新生成成功。
- 解析生成前后的 slnx：默认 26 个项目、bootstrap 5 个项目全部归组，
  项目路径/ID、依赖、启动元数据和解决方案配置一致，无顶层散落项目。
  默认分组含 Apps 1、Engine 12、Tests 8、Examples 2、CMake 3 个项目。
- `cmake --build --preset windows-debug --target dk_run --parallel 4` 成功；
  `ctest --preset windows-debug -R '^dk\.(bootstrap\.version|runtime\.)' --output-on-failure`
  版本、stdio、批处理重载及错误处理 4/4 通过。
- bootstrap Debug 构建和 CTest 1/1 通过；文档链接/元数据/编号检查及 git diff --check 通过。
- `python -X utf8 .../skill-creator/scripts/quick_validate.py .agents/skills/decker-spec-workflow` 通过。
- 本次仅变更 IDE 分组和文档流程，未重复 Release 构建及全量单元测试；
  已验证生成文件的分组结构，未操作 VS 界面。用户接受重新加载提示后即可显示。

## 遗留与下一步

下一里程碑仍为 M4.1；本记录涵盖 VS 工程生成和展示维护。
