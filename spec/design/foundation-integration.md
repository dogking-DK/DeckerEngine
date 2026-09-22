---
module: foundation-integration
created_at: "2026-09-22T11:52:43+08:00"
updated_at: "2026-09-22T11:52:43+08:00"
status: accepted
---

# Foundation CPU 集成验收（M1.6）

## 目标与边界

串联已有 Core/数学/IO，交付独立 dk-foundation-demo 示例及跨进程 CTest。
示例只使用公开接口，不扩展 dk-run，不建立 Scene/ECS 或正式场景文件协议。
本次完成后验收 M1；按用户授权继续依次开发 M2.1–M2.4，每节独立设计、留档、验证和提交。

## 模块与构建

examples/foundation/src/main.cpp 实现 dk_foundation_demo，输出 dk-foundation-demo。
PRIVATE 链接 dk::core、dk::math、dk::io；日志启用时额外链接 dk::logging。
DK_BUILD_EXAMPLES 默认 ON；只有 math/io 均开启时构建该例，缺少依赖时明确跳过，
不自动打开关闭的模块。bootstrap 显式关闭示例；无新增三方库或依赖组。
测试与 runner、Catch2 单元测试开关独立；DK_BUILD_TESTS=OFF 仍可构建和手动运行示例。

## 命令与示例数据

- dk-foundation-demo save <existing-project-root> <relative-file>
  生成非 nil EntityId，构造父/子 TRS，组合完整仿射矩阵（含非均匀缩放与旋转）；
  检查组合与逐次作用一致；编码、原子保存、重新读取并比较 ID/矩阵。
  命令明确覆盖指定目标，不创建根目录或父目录。
- dk-foundation-demo load <existing-project-root> <relative-file>
  只读解码，再计算固定局部点 (1,2,3) 的世界坐标和逆变换，核验点往返。
- --help 返回 0；无效命令/参数返回 2；业务/IO/解析失败返回 1。
  成功 stdout 是稳定的 entity_id/world_point/local_point/roundtrip 四行，
  save 与另一个进程 load 输出一致。日志与错误到 stderr，不混入 stdout。
  Windows 使用 wmain 接收 Unicode 路径；其他平台入口保留，安全保存遵循 IO 的 not_supported。

示例专用文本格式（最多读取 4096 字节）：

    DK_FOUNDATION_SAMPLE 1
    entity <canonical UUID>
    matrix <16 个 double，按数学行顺序>

输出使用 classic locale 和 max_digits10，按元素写入，不转储 Eigen/对象内存。
输入按 ASCII 空白分词，以 from_chars 严格解析完整数值；拒绝截断、额外 token、
非法/nil UUID、非有限值、非仿射末行及不可逆矩阵。
未知版本为 not_supported，其他格式问题为 invalid_argument，奇异矩阵沿用数学 invalid_state。
格式只服务当前集成样例，不兼容承诺为未来 M2 场景/工程格式。

## 所有权、失败与取舍

示例状态仅由局部值持有，无异步任务或共享可变状态。错误按调用层补充上下文。
保存前先完成编码与数值验证；保存后读回核验失败不代表保存未发生，不回滚已提交文件。
日志失败回退 stderr，不把日志当保存事务的一部分。
工程路径、符号链接、并发和断电边界沿用 foundation-io；不扩大跨平台承诺。

## 验证计划

CTest 驱动实际示例进程，各案例创建构建目录内独立测试工程（含中文/空格/emoji）。
验证 save/load 跨进程 ID、矩阵派生结果一致、二次保存覆盖且无临时残留；
验证损坏文件（ID/nil/版本/截断/非有限/非仿射/奇异/尾随数据/超限）、
缺失文件/父目录、越界路径、参数错误的退出码、stderr 上下文、stdout 留空且不改写文件。
全量 Debug/Release 回归，同时新建无日志/runner/Catch2 的 CPU 示例配置，
分别验证 Debug/Release，仅依赖 stduuid、Eigen 及 vcpkg 构建辅助包。
重新检查 bootstrap 示例关闭与版本探针；既有符号链接权限跳过仍如实记录。

## 相关记录

- [Core](foundation-core.md)、[数学](foundation-math.md)、[IO](foundation-io.md)
- [0008 集成验收](../development/0008-foundation-integration.md)
- [charconv](https://learn.microsoft.com/en-us/cpp/standard-library/charconv-functions?view=msvc-170)
- [Windows 命令行入口](https://learn.microsoft.com/en-us/cpp/cpp/main-function-command-line-args?view=msvc-170)
