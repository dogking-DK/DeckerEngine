---
module: physics-xpbd
created_at: "2026-10-09T10:33:53+08:00"
updated_at: "2026-10-09T11:20:00+08:00"
status: accepted
---

# CPU XPBD 距离约束参考求解器

## 范围与模块

M10.2 选择单一 XPBD 算法，演示固定上沿的布片在重力下垂落并接触水平地面。
`engine/physics/xpbd` 提供 `dk::physics_xpbd`，PUBLIC 仅 Core，使用标准库连续数组和 float32 运算；
不依赖 ECS、Eigen、Vulkan、Graph、窗口或文件 IO，不新增三方库。
SimulationServices PUBLIC 链接它；粒子不对应 Scene Entity，编辑/Play 场景文档保持 M10.1 隔离语义。
GPU 求解与独立绘制见 [GPU XPBD](physics-xpbd-gpu.md) 和 [模拟渲染](render-simulation.md)；CPU模块自身仍无GPU依赖。

## 数据、输入与执行顺序

公开 XpbdSolver 接受 XpbdConfig、positions、velocities、distance constraints；create 接收拥有型数组，
返回经验证和分组的实例。每个粒子索引稳定，positions=(x,y,z,inverse_mass)、velocities=(x,y,z,0)，
均为 alignas(16)、16-byte float32 记录；约束=(uint32 a,b,float rest_length,compliance)，同为16-byte。
逆质量为0表示固定粒子，固定位置不变、速度为0。位置/速度/约束各自连续存储，不传递 ECS 句柄。
约束按输入顺序做确定性贪心着色，再按色稳定分组；color_offsets 包含末尾偏移。
每色内所有端点均不共享（含固定点）；每轮按色递增执行，与GPU每色并行、色间同步使用同一顺序。
每拍 lambda 清零，拍内按约束累计；lambda 和原位置为内部工作数组，不是持久检查点。
只读 spans 在下次 advance/析构时失效；snapshot 返回独立拥有型数组及配置/指标，不暴露可变引用。

限制：1–1024粒子、0–8192约束、最多32色；拒绝坏索引、自环、重复无向边、初始重合端点、
非有限数值；初始坐标/速度分量绝对值<=1e6、逆质量0–1e6、rest_length 1e-6–1e6、
compliance 0–1e6。固定点不能在地面以下或带非零速度。
config：iterations 1–32，gravity_y -1000–1000，floor_y -10000–10000，damping 0–100 /s。
每拍 dt 1000000–33333333 ns，count 1–10000；每次调用的
count * (particles + iterations * (particles + constraints)) 不得超过20000000，避免同步请求无界占用owner。

## 数值方法和边界

参考 [XPBD 原论文](https://mmacklin.com/xpbd.pdf) Algorithm 1、式(17)(18)。对每个固定拍：
先预测 v=(v+dt*g)/(1+damping*dt)，x_pred=x+dt*v；固定点保持原位。
距离 C=|x_a-x_b|-rest，n=(x_a-x_b)/length，alpha_tilde=compliance/dt²，
DeltaLambda=(-C-alpha_tilde*lambda)/(w_a+w_b+alpha_tilde)；
x_a+=w_a*n*DeltaLambda，x_b-=w_b*n*DeltaLambda，lambda+=DeltaLambda。
双固定约束跳过。动态重合端点使用该边初始单位方向，避免除零或永久跳过约束。
每轮距离约束之后将动态粒子投影到 y>=floor_y，最后重建 v=(x_new-x_old)/dt；
接触地面时清除向下速度。地面无摩擦、无反弹、无半径；不支持自碰撞、弯曲、撕裂或三角形碰撞。
速度阻尼为显式的全局衰减，不声称实现原论文的 Rayleigh 约束阻尼；有限轮数仍存在约束残差。

## 布片输入与可观察指标

ClothConfig 默认 columns=8、rows=8、spacing=0.15 m、height=0.75 m、particle_mass=0.1 kg、
compliance=1e-6 m/N、iterations=12、gravity_y=-9.81、floor_y=0、damping=0.5 /s、seed=1。
columns/rows 2–32，spacing 0.01–1，height -100–100且>floor_y，mass 0.001–100，compliance 0–0.01；
其余复用 XpbdConfig 范围。索引 row*columns+column；初始为 XZ 平面，X以0居中，Z从0递增，row=0全固定。
非固定点的Y加入不超过0.01*spacing的正扰动；32位LCG(state=1664525*state+1013904223 mod 2^32)，
每个非固定点取一次更新后高24位/2^24。seed完整uint32，同参数同构建得到相同初始数组。
结构边为水平/垂直邻边，每格加入两条剪切对角线；rest取扰动后真实初始距离，无预应力。

每次成功提交更新 metrics：particle_count、constraint_count、color_count、max_constraint_error、
rms_constraint_error（米）、max_relative_error、max_speed（m/s）、kinetic_energy、
gravity_potential_energy（地面为零势能）、compliant_energy（只计compliance>0的 C²/(2*compliance)，焦耳）、
min_height、max_penetration、max_pin_displacement（米）。归约使用double，不承诺能量守恒或跨设备逐位相同。

## 状态与提交点

advance 在独立候选位置/速度上执行全部 N 拍。每拍检查位置/速度有限且分量绝对值<=1e6；
数值失败返回 invalid_state，参数/容量/工作预算失败为 invalid_argument，原数组/指标不变。
成功才交换数组/指标。OOM向宿主抛出，不伪装成可恢复物理错误。
SimulationService 同时准备候选FixedStepClock，求解成功后才提交时钟；严格step失败不改Play、时钟或Edit。
自动pump失败沿用M10.1冻结paused并记录fault，保留原物理状态和计数；stop/start才可恢复。

## 接入和验证计划

simulation.start 增加可选 solver="none"（默认）/"xpbd_cpu" 与 cloth；cloth仅允许xpbd_cpu。
旧时钟模式及步长范围保持兼容；XPBD限制dt<=33333333 ns。query结果增加 solver、cloth、metrics；
新增 simulation.particles(run_id,offset=0,limit=128) 查询拥有型粒子页，返回run_id/steps/时间，防止拼接不同拍。
Runtime::read_play_particles(run_id) 返回完整拥有型物理快照。Luau query允许粒子查询；Python沿用Client.call。
配置仍为会话内状态；不写scene文件，不加入场景事务，无检查点或M9录制扩展。

验证解析单约束合规度与质量权重、自由落体离散解析解、重合恢复、固定点、地面无穿透/向下速度、
非法输入/容量/溢出及中途数值失败回滚；同seed同N和分片step一致、不同seed初值不同、分组无冲突。
默认8x8布片以dt=10000000 ns推进300拍：有限、固定点误差0、穿透<=1e-6 m、最大相对约束误差<0.08；
位置确实变化，输出指标与原始数组独立复算一致。Runtime/Luau/真实IPC验证步数与状态同步、暂停、Stop保留Edit。
不以GPU结果或视觉截图代替CPU数值验证；不声称完整布料材料模型。

相关：[Physics API](physics-api.md)、[0075](../development/0075-cpu-xpbd.md)。

## GPU 数据互通

拥有型 XpbdSnapshot 补充与有序约束对应的初始单位方向（float3 数组），用于 [GPU 求解](physics-xpbd-gpu.md) 在动态重合时复用相同退化规则。导出已经推进的 CPU 状态不重新定义方向；CPU 算法不变。
