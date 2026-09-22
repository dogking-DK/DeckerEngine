---
id: "0018"
created_at: "2026-09-22T15:58:55+08:00"
updated_at: "2026-09-22T16:01:55+08:00"
status: completed
design_refs:
  - ../design/project-foundation.md
---

# 0018 根目录 VS2026 工程生成脚本

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

## 遗留与下一步

下一里程碑仍为 M4.1；本次只生成 VS 工程。
