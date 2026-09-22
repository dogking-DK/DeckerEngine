---
id: "0004"
created_at: "2026-09-22T10:28:51+08:00"
updated_at: "2026-09-22T10:42:53+08:00"
status: completed
design_refs:
  - ../design/foundation-math.md
  - ../design/project-foundation.md
---

# 0004 Eigen 基础数学与里程碑细分

## 目标与设计依据

按用户要求使用 Eigen，并把 M0–M10 拆为可独立验收的小阶段。
本次完成 [数学设计](../design/foundation-math.md) 中的 M1.2；
TRS Transform 属于下一小阶段 M1.3。实施前先建立设计并细分 Roadmap。
保留此前尚未提交的 Core/UUID 工作区改动，未改写历史记录或创建 Git 提交。

## 实际变更

- [Roadmap](../roadmap.md) 为 M0–M10 各里程碑增加小阶段表，
  明确范围、前置、验收和状态；已完成工作链接原有证据。
  M1 拆为 Core、Eigen 基础、Transform、IO、安全保存、集成验收。
  [spec 规范](../README.md) 增加默认单次推进一个小阶段的规则，
  现有 workflow skill 通过读取该规范供后续开发遵循。
- [Types.hpp](../../engine/foundation/math/include/dk/math/Types.hpp)
  提供 float/double 的 Vec2/3/4、Mat3/4、Quat 透明 Eigen 别名。
  定义右手系、+Y 上、-Z 前、米/秒/弧度、列向量/列主序约定，
  明确默认初始化、四元数系数顺序、表达式求值、对齐与持久化边界。
- [Math.hpp](../../engine/foundation/math/include/dk/math/Math.hpp) 和
  [Math.cpp](../../engine/foundation/math/src/Math.cpp) 提供 constexpr 角度转换、
  检查过的 Vec3/Quaternion 归一化和轴角旋转。
  零/非有限参数返回 invalid_argument；归一化先按最大分量缩放，
  使用逐分量除法避免极小数倒数溢出，再由 Eigen 归一化。
- [数学 CMake](../../engine/foundation/math/CMakeLists.txt) 创建 dk_math / dk::math，
  PUBLIC 链接 dk::core、Eigen3::Eigen，消费者无需重复寻找 Eigen。
  [Options](../../cmake/Options.cmake) 增加默认 ON 的 DK_BUILD_MATH；
  [分组目录](../../engine/foundation/CMakeLists.txt) 按开关创建模块。
- [vcpkg 清单](../../vcpkg.json) 从 foundation 移除未使用的 glm，
  新增 math feature 使用 eigen3。基线仍为
  62159a45e18f3a9ac0548628dcaf74fcb60c6ff9，实际安装版本 5.0.1。
  [Vcpkg.cmake](../../cmake/Vcpkg.cmake) 在启用数学时自动补充 math。
  [预设](../../CMakePresets.json) 中 bootstrap 关闭数学，
  desktop-deps 列入 math；开发/Ninja 默认开启数学。
- [MathTests.cpp](../../tests/unit/MathTests.cpp) 新增 9 组行为各覆盖 float/double，
  由独立 dk_math_tests 执行，共 18 项；测试 target 只显式链接 dk::math 和 Catch2。
  同步 README、架构、工程/数学设计、索引和 Roadmap 的状态与下一步。

## 验证记录

环境：Windows x64、CMake 4.2.1、Visual Studio 18 2026、
MSVC 19.51.36257.0；vcpkg 依赖工具链 MSVC 19.44.35207。
所有本次构建均开启 DK_WARNINGS_AS_ERRORS。
vcpkg 提示源构建路径较长，但 Eigen 安装、编译、链接全部成功，无须修改路径或压制警告。

| 命令 | 实际结果 |
| --- | --- |
| `cmake --preset windows-dev -DDK_WARNINGS_AS_ERRORS=ON` | 通过；安装 Eigen 5.0.1，移除 glm |
| `cmake --build --preset windows-debug`、`ctest --preset windows-debug` | 通过，36/36 |
| `cmake --build --preset windows-release`、`ctest --preset windows-release` | 通过，36/36 |
| `cmake --preset windows-bootstrap -DDK_WARNINGS_AS_ERRORS=ON` | 通过；继续仅 stduuid 和 vcpkg CMake 辅助包 |
| `cmake --build --preset windows-bootstrap-debug`、`ctest --preset windows-bootstrap-debug` | 通过，1/1 |
| 下列独立配置、构建和测试 | 通过，28/28；没有日志、runner 或 GPU 依赖 |

独立数学验证使用新目录，不覆盖默认开发预设的缓存：

```powershell
cmake --preset windows-dev -B out/build/windows-math-only -DDK_BUILD_LOGGING=OFF -DDK_BUILD_RUNNER=OFF -DDK_VCPKG_FEATURES= -DDK_WARNINGS_AS_ERRORS=ON
cmake --build out/build/windows-math-only --config Debug
ctest --test-dir out/build/windows-math-only -C Debug --output-on-failure
```

此配置仅安装 Eigen、stduuid、Catch2 和 vcpkg CMake 辅助包，
实际编译了只链接 dk::math/Catch2 的消费者，验证 PUBLIC include/link 传递。
开发构建 36 项 = 18 项数学 + 16 项 Core + 日志流分离 + 版本探针；
独立配置 28 项 = 18 项数学 + 10 项无日志 Core。

数学覆盖角度符号/整周、手性/复合顺序、齐次点与方向、列主序存储、
零/NaN/Inf 拒绝、最大有限数/最小正规数/最小次正规数归一化、
四元数构造/存储顺序、q/-q 旋转等价、轴角右手旋转及标准容器使用。
Debug/Release 均不依赖 assert 判断这些行为。

收尾检查通过：2 份 JSON 可解析，137 个本地链接有效，9 组文档时间元数据有效；
M0–M10 共 46 个不重复的小阶段，前置阶段编号可解析；
核对安装清单确认 bootstrap 不含 Eigen、独立数学不含日志/JSON/GLM。
git diff --check 通过。

## 偏差与决策

- Eigen 5.0.1 是已有锁定基线中的版本，未变更基线、添加下载脚本或引入 BLAS/LAPACK。
- 不用任意 epsilon 拒绝非零向量；输入小并不等同于非法，先缩放可安全计算单位方向。
- math 是独立可选模块，不让最小 Core 探针额外安装 Eigen。
- 单次只实现 M1.2；数学库接入不代表 Transform 已完成，也不代表整个 M1 完成。
- 设计接受 PUBLIC Eigen 类型，避免包装全部线性代数 API。
  函数边界要求具体同精度类型；Eigen 惰性表达式由调用方先求值。

## 遗留问题与下一步

本次范围已完成。下一小阶段 M1.3 先扩展数学设计：
明确 TRS、parent * local、剪切保留、非均匀/负/零缩放和逆变换失败语义，
再实现 Transform 并新增行为验证。下一开发编号预计 0005，开工时重新扫描确认。

未验证 Linux/macOS、其他编译器/指令集、fast-math/非默认浮点环境、
全桌面依赖和关闭 vcpkg 的外部 Eigen 安装配置；
独立数学及 bootstrap 本次仅实测 Debug，完整开发配置覆盖 Debug/Release。
不把这些未验证项计入 M1.2 当前 Windows 工具链验收以外的支持承诺。

## 修改记录

- 2026-09-22T10:28:51+08:00：先建立数学设计与开发记录，细分 Roadmap。
- 2026-09-22T10:39:50+08:00：完成 Eigen 实现、三种构建配置验证及文档状态同步。
- 2026-09-22T10:42:53+08:00：完成文档、阶段编号、依赖边界和差异检查。
