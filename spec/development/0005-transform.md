---
id: "0005"
created_at: "2026-09-22T10:47:13+08:00"
updated_at: "2026-09-22T10:59:01+08:00"
status: completed
design_refs:
  - ../design/foundation-math.md
---

# 0005 Transform TRS、组合与逆变换

## 目标与设计依据

实施 Roadmap M1.3，先扩展 [数学设计](../design/foundation-math.md)，
确定 TRS 输入、完整仿射矩阵存储、父子组合和数值求逆策略。
保留既有未提交改动，未创建 Git 提交；M1.4 文件 IO 不在本次范围。

## 实际变更

- [Transform.hpp](../../engine/foundation/math/include/dk/math/Transform.hpp)
  新增 `BasicTrs<Scalar>`、`BasicTransform<Scalar>`，仅支持 float/double。
  Trsf/Trsd、Transformf/Transformd 显式区分精度，Trs/Transform 默认 float。
  TRS 有安全的初值，Transform 默认单位矩阵；后者只暴露只读矩阵。
- [Transform.cpp](../../engine/foundation/math/src/Transform.cpp)
  实现 from_trs、from_matrix、compose、inverse、transform_point、transform_direction。
  四元数归一化复用 M1.2；T * R * S、parent * local 保持列向量约定。
  仿射矩阵完整保留剪切，负缩放保持镜像；零缩放允许正向使用、禁止求逆。
- 逆变换对 3×3 线性部分按最大绝对分量缩放，Eigen FullPivLU 使用
  64 * epsilon 的相对主元阈值，恢复逆线性部分后计算逆平移。
  不用全矩阵行列式的绝对阈值；没有添加伪逆或 TRS 近似分解。
- 所有输入及算术结果检查有限性：非法参数返回 invalid_argument，
  奇异/数值秩不足或溢出返回 invalid_state，错误携带操作名。
  失败不修改输入、不发布半完成对象，不依赖 assert 进行外部参数校验。
- [模块 CMake](../../engine/foundation/math/CMakeLists.txt) 注册实现和公开头；
  [测试 CMake](../../tests/unit/CMakeLists.txt) 把
  [TransformTests.cpp](../../tests/unit/TransformTests.cpp) 加入 dk_math_tests。
  依赖版本、vcpkg feature、开关和预设均未改变。
- 更新 README 的 API 示例、架构/数学设计、索引和 Roadmap：
  M1.3 已完成，M1 保持进行中，下一项为 M1.4，下一编号预计 0006。

## 验证记录

使用现有 Windows x64 配置，MSVC 19.51.36257.0、CMake 4.2.1、
Eigen 5.0.1、Catch2 3.13.0；开发与独立数学缓存均核实 DK_WARNINGS_AS_ERRORS=ON。

| 实际命令 | 结果 |
| --- | --- |
| `cmake --build --preset windows-debug` | 成功；自动重新配置，无新增依赖 |
| `ctest --preset windows-debug` | 60/60 通过 |
| `cmake --build --preset windows-release` | 成功 |
| `ctest --preset windows-release` | 60/60 通过 |
| `cmake --build out/build/windows-math-only --config Debug` | 成功；日志与 runner 关闭 |
| `ctest --test-dir out/build/windows-math-only -C Debug --output-on-failure` | 52/52 通过 |

新增 12 组测试各实例化 float/double，共 24 项；
完整开发配置 60 项 = 原 36 项 + 24 项；
独立数学配置 52 项 = 原 28 项 + 24 项。

验证覆盖：默认单位值；手算 T/R/S 顺序与点/方向区别；
非单位四元数归一化和 q/-q 等价；非均匀缩放/旋转产生的剪切；
三级组合与逐级应用一致；仿射输入所有权；剪切矩阵左右逆及点/方向往返；
负缩放镜像；部分/全部零缩放正向成功但求逆失败；
线性相关和相对主元阈值两侧的数值秩判断；
极小/极大均匀缩放；NaN/Inf/非仿射矩阵拒绝；
组合、点/方向、倒数及逆平移溢出时返回错误并保留输入。

收尾检查通过：151 个本地文档链接、10 组创建/更新时间元数据有效；
git diff --check 及新增 Transform 文件的空白检查通过，
Roadmap 与开发索引均已标记 M1.3/0005 完成。

未重跑本次不受影响的 bootstrap（数学关闭）；
未验证独立配置 Release、Linux/macOS、其他编译器/指令集及非默认浮点模式。
先前 0004 的验证保持历史记录，不视为本次新增执行结果。

## 偏差与决策

本次实现符合先行设计。
TRS 作为可编辑输入，组合结果采用仿射矩阵，避免非均匀缩放的剪切损失。
未提供 rotation/scale 分解访问器，也不把方向接口当法线变换。
求逆阈值是当前精度的固定数值策略；病态矩阵可能虽数学可逆仍被拒绝。
极端数据允许浮点舍入/下溢，不能表示的非有限结果明确失败。

## 遗留问题与下一步

M1.3 范围已完成。下一项 M1.4 先建立 foundation-io.md，
明确 Unicode 路径、工程相对路径、文件字节读写与错误映射。
原子替换/安全保存留到 M1.5，当前不提前实现。
投影、法线、TRS 分解和 Scene 层级管理不在本阶段交付范围。

## 修改记录

- 2026-09-22T10:47:13+08:00：先扩展设计并建立记录。
- 2026-09-22T10:55:54+08:00：完成实现、Debug/Release 与独立数学回归，更新阶段状态和交接信息。
- 2026-09-22T10:59:01+08:00：完成文档链接、时间元数据和差异检查。
