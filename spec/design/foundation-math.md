---
module: foundation-math
created_at: "2026-09-22T10:28:51+08:00"
updated_at: "2026-09-22T10:55:54+08:00"
status: accepted
---

# Foundation Math：Eigen 基础数学与 Transform

## 目标与本次范围

按用户要求使用 Eigen，实施 [Roadmap](../roadmap.md) 的 M1.2–M1.3。
M1.2 已提供类型入口、几何约定、角度和旋转；M1.3 已完成
Transform 的 TRS、组合、逆变换及奇异缩放处理。
本次不实现投影/相机、几何查询、公开矩阵分解接口或 GPU 数据布局。

## 模块边界与依赖

公开入口位于 engine/foundation/math/include/dk/math，实现在 src。
新增静态库 dk_math / dk::math，PUBLIC 链接 dk::core 与 Eigen3::Eigen；
Result 出现在公开函数中，Eigen 类型作为透明别名供消费者直接使用。
不再引入 GLM，不重新封装 Eigen 运算符或复制线性代数算法；
不依赖 Scene、SDL、Vulkan、日志或运行时服务。

新增 DK_BUILD_MATH（默认 ON）；关闭时不创建数学 target/测试。
vcpkg 新增 math feature 安装 eigen3，启用数学时在 project() 前自动选择；
foundation 中移除尚未使用的 glm。基线不变，当前 eigen3 为 5.0.1。
windows-dev/Ninja 默认构建数学；bootstrap 显式关闭数学，继续只安装 stduuid。
只开启数学而关闭日志时，无须安装 fmt/spdlog/JSON（须清空额外 feature）。

## 数值与空间约定

- 长度为米、时间为秒、几何 API 角度为弧度；角度转换函数名称明确输入单位。
- 右手系：+X 向右、+Y 向上、默认观察方向 -Z；X cross Y = +Z。
  绕 +Z 正向旋转 90 度把 +X 变为 +Y，遵循右手定则。
- 列向量，作用顺序为 M * v；矩阵明确采用列主序。
  复合矩阵 A * B 先执行 B；父子变换采用 parent * local。
  齐次点 w=1，方向 w=0；本阶段以测试验证这些约定。
- Vec2f/Vec3f/Vec4f、Mat3f/Mat4f、Quatf 使用 float；
  同名 d 后缀类型使用 double。上层实时场景默认 float，
  数值参照计算可显式选择 double，无全局 Scalar 切换宏。
- Eigen 默认构造不会保证零/单位值；必须显式使用 Zero()/Identity() 或赋值。
  持久化逐项写入数值，不直接复制对象内存。
- Quaternion 构造参数为 w,x,y,z，coeffs() 为 x,y,z,w；
  旋转前要求单位四元数。q 与 -q 表示相同旋转，不把分量相等当旋转等价。
- 保留 Eigen 默认对齐和向量化，不添加全局 Eigen 宏。
  C++23 标准容器使用正常 allocator；不把这些对象直接用作 shader buffer ABI。
  使用具体类型或完整表达式 .eval() 保存运算结果，避免悬空表达式和隐式重复计算。

## 接口与数据

Types.hpp 提供上述 Eigen 类型别名；Math.hpp 提供：

| 接口 | 语义 |
| --- | --- |
| radians(degree_value)、degrees(radian_value) | 浮点模板、constexpr；只换算，不裁剪、不环绕；非有限值遵循浮点运算 |
| normalize_vector(Vec3f / Vec3d) | 返回 Result<Vec3f / Vec3d>，保留方向并归一化 |
| normalize_quaternion(Quatf / Quatd) | 返回单位四元数，保留分量的共同符号 |
| rotation_from_axis_angle(Vec3f / Vec3d, 同精度 radians) | 归一化非零轴，以 Eigen AngleAxis 构造单位四元数；右手主动旋转 |

向量/四元数归一化先拒绝非有限分量和全零输入，再按最大绝对分量缩放后
交由 Eigen 计算范数，避免直接求原始平方和造成溢出或下溢。
不采用任意世界单位 epsilon 拒绝很小但合法的方向；极小/极大有限输入应可归一化。
浮点环境保持工具链默认，不启用 fast-math 或 flush-to-zero。
轴角接口拒绝 NaN/Inf 角度，即使角度为零也要求合法的非零轴。
普通非法输入返回 ErrorCode::invalid_argument，错误携带操作名；不依赖 assert。
成功结果为独立的值，不返回引用、Map 或 Eigen 惰性表达式。
检查函数的向量参数使用具体 Vec3f/Vec3d；先将 Eigen 表达式求值到对应类型，
避免 float/double 重载之间的隐式转换歧义。轴和角度使用相同精度。
不接管所有 Eigen 原生操作的参数检查；这些检查仅保证上述 dk 函数的契约。

## M1.3：TRS 与仿射 Transform

### 表示与接口

新增 Transform.hpp / Transform.cpp，仍属于 dk::math，无新依赖或构建选项。
只实例化 float/double；命名为 Trsf/Trsd、Transformf/Transformd，
Trs / Transform 别名默认 float。模板实现放 cpp，并显式实例化两种精度。

`BasicTrs<Scalar>` 是编辑/构造数据，字段为 translation、rotation、scale，
默认 Zero、Identity、Ones；字段可修改，合法性由构造 Transform 时检查。
`BasicTransform<Scalar>` 保存私有 4×4 仿射矩阵，默认单位阵；
没有可变矩阵引用、缓存或全局状态。

| 操作 | 契约 |
| --- | --- |
| `from_trs(trs) -> Result<Transform>` | 验证平移/缩放有限、四元数有限且非零并归一化；构造 T * R * S |
| `from_matrix(matrix) -> Result<Transform>` | 接受有限仿射矩阵，末行必须精确为 [0,0,0,1]；可含剪切、镜像或奇异线性部分 |
| matrix() -> const Matrix4& | 只读矩阵，引用有效期受 Transform 对象生命周期约束 |
| `parent.compose(local) -> Result<Transform>` | 得到 parent * local；先 local 后 parent，输入保持不变 |
| `transform_point(point) -> Result<Vector3>` | L * point + translation，等价于 w=1 |
| `transform_direction(direction) -> Result<Vector3>` | L * direction，等价于 w=0；保留缩放长度、不自动归一化 |
| `inverse() -> Result<Transform>` | 返回仿射逆，奇异、数值秩不足或结果不能表示时返回错误 |

TRS 应用顺序为先缩放、再旋转、最后平移；世界变换为 parent * local。
方向接口适用于向量而非法线；法线的逆转置、投影、相机、TRS 分解与插值不在本次范围。
q 和 -q 构造相同旋转；不强制四元数符号。
输入 TRS 和 matrix 按值复制到结果，不保留参数引用。

### 缩放、剪切与组合

负缩放产生镜像，允许构造、组合和求逆；零缩放可压扁对象，
其正向点/方向变换有效，但不存在完整逆变换。
非均匀缩放与不同方向旋转的组合通常产生剪切：
Transform 保留完整线性矩阵，不把组合结果近似写回 TRS，
也不提供误导性的 rotation()/scale() 分解访问器。
层级/脏标记/ECS 组件属于 M2；本阶段仅提供单值数学操作。

### 求逆与错误策略

不以 4×4 行列式的绝对值判断奇异性，以免单位缩放或较大平移影响判断。
取 3×3 线性部分的最大绝对分量 s；s=0 直接失败。
逐分量除以 s，使用 Eigen FullPivLU 对缩放后的线性部分求逆，
显式设置相对主元阈值 `64 * numeric_limits<Scalar>::epsilon()`。
任何主元未超过最大主元乘此阈值时，按数值秩不足拒绝求逆；
这是固定的数值策略，不是业务世界单位 epsilon，也不提供伪逆。
均匀极小/极大缩放不会仅因绝对大小被拒绝。

将归一化线性逆逐分量除以 s，得到 L_inv，再计算 -L_inv * translation。
整个过程检查结果有限；若倒数或平移计算溢出，返回错误。
不承诺任意病态矩阵的逆均能返回；修改阈值或增加可配置精度策略需另写设计与验证。

输入 NaN/Inf、零四元数或非仿射末行返回 invalid_argument。
合法输入的算术溢出、奇异/数值不可逆状态返回 invalid_state，并附操作名上下文。
构造、组合、求逆和点/方向计算都使用 Result，失败不修改任一输入；
不返回含 NaN/Inf 的成功结果。结果发生有限精度舍入/下溢遵循默认浮点环境，
不声称任意极端数据都能精确往返。不捕获 bad_alloc，不用断言替代输入验证。

### M1.3 实施与验收

先扩展本设计并建立 0005；再添加类型/实现、注册公开头和测试源。
float/double 都验证默认单位值、明确的 T/R/S 顺序、四元数归一化、
父子和三级组合、含剪切矩阵的保留与往返逆、镜像、
零缩放正向可用/求逆拒绝、病态矩阵拒绝、极端均匀缩放、
非有限/非仿射输入拒绝、组合/点/方向/求逆溢出错误和输入不变性。
项目 Debug/Release 全量回归，另验证关闭日志/runner 的数学配置。
M1.3 完成后下一项为 M1.4 文件 IO，不在本次实现 IO。

M1.3 已验收：Windows Debug/Release 各 60 项通过，
独立数学 Debug 配置 52 项通过；新增 12 组 float/double 行为共 24 项测试。
详见 [0005](../development/0005-transform.md)。

## 生命周期、并发与错误

值类型无共享可变状态，函数不保留参数引用，可由多个线程独立调用。
输入不会被修改。内部固定大小运算无需资源生命周期管理；
构造错误诊断时仍可能分配，不宣称 noexcept 或捕获 bad_alloc。
可恢复参数错误使用 dk::Result；调用方不得在未检查结果时访问值。

## M1.2 实施与验证（历史基线）

1. 先写本设计、细分 Roadmap 并建立 0004 记录。
2. 接入 Eigen feature、构建开关与 dk::math，提供类型、换算和检查过的旋转操作。
3. 单独 dk_math_tests 仅链接 dk::math / Catch2，以验证 PUBLIC 传递依赖。
4. float/double 验证坐标手性、列向量/列主序、点与方向、复合顺序、
   角度往返、归一化、极大/极小输入、非有限/零失败路径、四元数顺序、
   q/-q 旋转等价和标准容器使用。
5. Windows Debug/Release 开启项目 warnings-as-errors，运行 Core 与数学全部测试；
   额外验证关闭数学的 bootstrap 及无日志的独立数学配置。
6. 完成 M1.2 后停在明确交付边界；M1.3 先扩展设计再实施，不将整个 M1 标为完成。

M1.2 已完成；Windows 开发 Debug/Release 各 36 项测试通过，
关闭日志/runner 的独立 Debug 配置 28 项通过，bootstrap Debug 1 项通过。
实际命令、环境与未验证范围见 0004。

## 参考与记录

- [Eigen CMake 集成](https://libeigen.gitlab.io/eigen/docs-5.0/TopicCMakeGuide.html)
- [Eigen Quaternion](https://libeigen.gitlab.io/eigen/docs-5.0/classEigen_1_1Quaternion.html)
- [Eigen 存储顺序](https://libeigen.gitlab.io/eigen/docs-5.0/group__TopicStorageOrders.html)
- [Eigen 表达式注意事项](https://libeigen.gitlab.io/eigen/docs-5.0/TopicPitfalls.html)
- [Eigen 标准容器](https://libeigen.gitlab.io/eigen/docs-5.0/group__TopicStlContainers.html)
- [Core 设计](foundation-core.md)
- [0004 Eigen 基础数学](../development/0004-eigen-math-foundation.md)
- [0005 Transform](../development/0005-transform.md)
- [Eigen FullPivLU](https://libeigen.gitlab.io/eigen/docs-5.0/classEigen_1_1FullPivLU.html)
