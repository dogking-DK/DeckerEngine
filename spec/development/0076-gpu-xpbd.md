---
id: "0076"
created_at: "2026-10-09T10:59:13+08:00"
updated_at: "2026-10-09T11:36:11+08:00"
status: completed
design_refs:
  - ../design/physics-xpbd-gpu.md
  - ../design/render-simulation.md
  - ../design/physics-xpbd.md
  - ../design/physics-api.md
  - ../design/architecture.md
  - ../design/graphics-graph.md
---

# 0076 M10.3 GPU XPBD 与布片可视化

## 目标与实现

依据先行落盘的 [GPU求解设计](../design/physics-xpbd-gpu.md)、[布片渲染设计](../design/render-simulation.md)，
将M10.2同一算法接入Graph；使用真实Vulkan同步验证和CPU数值对照，保持CPU-only Runtime。

- [physics/xpbd-gpu](../../engine/physics/xpbd-gpu) 提供 GpuXpbdSolver/GpuXpbdFrame。
  复用CPU配置、16-byte连续粒子/速度/约束、确定分色，并从快照导出原始退化方向。
  [Slang求解](../../shaders/physics/xpbd.slang) 为预测、分色约束、地面、速度重建四种64线程kernel。
  每拍清零lambda、拍内累计，每色独立Pass；固定点、质量、柔度、阻尼与地面规则同CPU。
- 每批先GPU复制已发布状态至候选buffer，图录制/提交成功后才发布新位置/速度和拍数。
  初态只上传一次；默认不读回、不等待。count=0..8、dt=1..33.333333ms、最多4096计算Pass；0拍用于暂停展示。
  显式readback/capture_plan提供数值和图/同步诊断；完成前拒绝读回，诊断拒绝发散值。
  成功提交不等于数值健康，异步数值发散不能撤销已提交批次；调用者停止并重建，未伪称CPU数值回滚保证。
- [render/simulation](../../engine/render/simulation) 提供独立ClothRenderer/SimulationFrame。
  同图追加颜色/深度清除、布片绘制、可选RGBA8读回；vertex shader直接读取候选位置，无CPU顶点回传。
  规则网格通过SV_VulkanVertexID生成三角形；正交相机、棋盘布片、网格地面和深度测试。
  位置最后一次计算写入经速度重建读取，再传到vertex读取，验证完整两段屏障链。
- GPU与CPU状态均不逐粒子创建Scene实体；Frame拥有不可变结果，旧帧不受后续推进影响。
  在途buffer、临时资源、descriptor、pipeline由提交队列保留；销毁公开对象或关闭时不提前释放。
- `DK_BUILD_PHYSICS_GPU`/`DK_BUILD_RENDER_SIMULATION`默认OFF，windows-graphics显式开启，PUBLIC/PRIVATE依赖明确。
  物理模块装配移到Render之前；不新增三方库、版本或vcpkg feature。
  [独立示例](../../examples/simulation/src/main.cpp) `dk-xpbd-demo [output-directory]` 输出初态/300拍PPM，覆盖规则写入[指南](../guides/simulation.md)。
  Runtime模拟命令仍为CPU后端，GPU命令/脚本统一实验在M10.4继续；本次没有编辑器Play视口或检查点。

## 密集图瓶颈与处理

300拍验收最初以8拍一图触发240秒超时（`out/verify/20261009-111839-468328cc`）；
进程采样约183秒CPU时间、420MB工作集。检查发现 [Graph编译](../../engine/graphics/graph/src) 对每条新依赖线性扫描去重，
DFS/调度/裁剪每节点扫描全部依赖；多色多迭代的密集读写放大了成本。

先更新 [Graph设计](../design/graphics-graph.md)，再将依赖改为批量排序去重，DFS/调度访问已排序的before区间，
裁剪使用contents/显式依赖的反向区间。成对危险分析与全部依赖原因保留，最终计划/顺序语义不变。
只优化Graph仍超时（`20261009-112433-7562b55c`）；求解器随后缓存最近一次无消费者的不可变计划，
以count/readback为键，固定拓扑和配置保证结构一致。dt仍为每次push constant，buffer/回调每次绑定。
缓存只在提交成功后替换，带渲染消费者的图单独编译。未提高超时、未放宽数值容差。

新增256个保留读写Pass、双mip重复依赖的回归，校验完整顺序及98,175条唯一危险/内容依赖，0.60秒通过。
最终GPU综合验收22.13秒，300拍布片case（含初始化/读回/诊断/断言）15.23秒；这是Debug端到端耗时，非GPU kernel基准或实时帧率保证。

## 验证命令和结果

Windows/MSVC Debug，NVIDIA GeForce RTX 4070 Laptop GPU，driver 596.49；固定依赖均已安装。

```powershell
cmake --preset windows-scripting
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_xpbd_tests','dk_simulation_tests','dk_run','dk_ctl') -TestRegex '^dk\.(xpbd\.|simulation\.)' -Reason '共享快照和CPU-only装配回归'
cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target @('dk_gpu_xpbd_probe','dk_graph_tests','dk_graph_probe','dk_xpbd_demo') -TestRegex '^dk\.(xpbd\.gpu_validation$|graph\.)' -Reason 'GPU求解/渲染与Graph密集依赖回归'
./out/build/windows-graphics/bin/Debug/dk-xpbd-demo.exe out/xpbd-gpu-demo
./scripts/check-spec.ps1
git diff --check
```

实际按失败影响分次重跑，保留此前未受影响结果，累计57个不同CTest用例通过、0跳过：

| 范围 | 结果与日志 |
| --- | --- |
| CPU XPBD、模拟时钟/服务/命令、真实stdio/pipe | 16通过；`out/verify/20261009-111320-f72e702f` |
| Graph原有声明/编译/裁剪/诊断/预算与无验证层GPU执行 | 37个CPU用例及1个GPU执行通过；`out/verify/20261009-112433-7562b55c`；该次另有待修复失败，不能称该批全通过 |
| 新增密集图回归 | 1通过；`out/verify/20261009-113015-34a84547`；该次GPU断言/loader提示失败，后续单独修复 |
| GPU XPBD综合验收及Graph同步验证 | 2通过，0失败/跳过；`out/verify/20261009-113313-09f4b2dc` |

GPU用例验证自由落体、柔性单边/质量权重、动态重合、地面、35粒子尾工作组、已推进CPU初态、缓存换dt、
数值越界诊断和Pass预算拒绝；300拍分批对比、0拍暂停、非法渲染输入、部分录制和驱动提交失败不发布状态，
超时/完成前读回拒绝、旧粒子/图像不变、无读回模式、在途销毁、分享图像及关闭排空。
验证错误/警告均为0，GPU与Memory分配回到0。

机器旧AMD隐式层在NVIDIA设备初始化时发出API 1.3/1.4 loader提示；旧Graph探针最初把它计为验证失败。
临时禁用该层仍产生“forced disabled”loader警告，故最终未依赖该环境覆盖。
两探针只将精确匹配的旧AMD API版本提示单列loader_warnings且保留日志；其他警告和全部VUID错误仍失败。
原有Graph验证最终显示errors=0、warnings=0、loader_warnings=2。

初轮还修复了命名空间歧义、已裁剪外部绑定、多拍Pass重名、shader顶点语义、附件初始化/访问和push-constant阶段声明。
同步断言从错误要求紧邻write→vertex，修正为实现中的write→velocity-read→vertex-read完整链。
图像检查修正默认相机深度方向，避免地面错误遮挡悬空布片；最终两幅图均已查看。

seed=42、8×8、dt=10ms、N=300的最终结果：

| 指标 | 实测 | 预定验收 |
| --- | --- | --- |
| 最大位置分量CPU/GPU差 | 1.057982444763e-5 m | ≤2e-3 m |
| 最大速度分量CPU/GPU差 | 3.129243850708e-5 m/s | ≤2e-2 m/s |
| GPU最大距离约束误差 | 0.000205868315018 m | <0.003 m |
| 最低高度/穿透/固定点位移 | 0 / 0 / 0 m | ≤1e-6 m边界；固定点完全一致 |
| 末帧布片像素/变化像素 | 33534 / 76372 | 各>1000 |

产物位于 `out/build/windows-graphics/test-artifacts/Debug/gpu-xpbd`：comparison.json、cloth-plan.txt、两幅PPM。
另将PPM无损转换成PNG用于查看；不作为逐像素跨设备golden。示例实际退出0，输出steps=300、simulated_time_ns=3000000000，
`out/xpbd-gpu-demo`生成两幅PPM，与探针两幅PPM的SHA-256分别一致。构建日志没有编译warning/error；
文档检查通过164个Markdown、1691个本地链接及索引/元数据/测试入口/JSON清单；空白检查通过。

## 边界与下一步

未运行全量、Release、其他GPU/平台或编辑器集成；CPU/GPU对照只承诺设计容差。
Graph仍有成对分析成本，含消费者的长图每次编译；最大预算不是任意配置的实时性保证。
本次M10.3验收完成，M10保持进行中，下一项M10.4统一GPU命令/脚本实验、配置/数值指标/图像导出。
