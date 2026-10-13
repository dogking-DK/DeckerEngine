# 大气质量与性能复现

固定协议见[大气设计](../design/render-atmosphere.md#质量与性能采集协议)。质量检查和性能采样分开，
性能需要优化构建。所有命令在仓库根运行，无新依赖，Python只用标准库。
本机交付结果见[2026-10-13质量/性能报告](../benchmarks/2026-10-13-atmosphere.md)，1.5ms候选目标未达。

## CPU-only与required GPU质量

```powershell
cmake --preset windows-scripting -DDK_BUILD_RENDER_ATMOSPHERE_REFERENCE=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting `
  -Target @('dk_atmosphere_tests','dk_run') `
  -TestRegex '^dk\.(atmosphere\.[^_]+|environment\.process)$' -Reason '纯CPU积分与持久化'

cmake --preset windows-graphics
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics `
  -Target @('dk_atmosphere_probe','dk_atmosphere_benchmark','dk_aerial_perspective_probe','dk_surface_sunlight_probe','dk_atmosphere_demo') `
  -TestRegex '^dk\.(atmosphere\.(reference|multiple|cache|vacuum|continuity|benchmark|demo)_gpu_validation|render\.(aerial_(numeric|composition|failures)|sunlight_(normals|direct|diffuse|failures))_gpu_validation)$' `
  -Reason '固定参考集、地平线连续性、观测与GPU寿命'
```

GPU配置必须有真实设备及Khronos/synchronization validation，不允许skip替代验收。
本机AMD switchable隐式层冲突时，只对当前命令临时设置`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，
用try/finally恢复原值；此项不关闭Khronos层。性能驱动只在子进程中设置该变量。

CPU参考库选项独立于DK_BUILD_RENDER_ATMOSPHERE；后者仍要求Graph/Shaders。
MS CPU高采样闭合在Debug中可能耗时较长，CTest保留1200秒上限。

## 三个预设与1080p矩阵

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_atmosphere_benchmark -TestRegex '^dk\.atmosphere\.benchmark_aggregation$' `
  -Reason '优化版性能探针和汇总契约'
python scripts/benchmark-atmosphere.py --build-dir out/build/windows-graphics
```

驱动执行18个独立进程：3轮×3预设×timestamp OFF/ON；轮换顺序。运行期间保持设备空闲，
不要同时执行其他GPU测试或修改相关源码。探针要求DK_ENABLE_PROFILING=OFF，不把未连接Tracy称为Tracy采集。
每进程按static、sky-update、physical-update、unlit顺序运行；各正常模式冷帧1、预热5、稳态40，物理重建3次。
Sky更新每帧升高0.1m；物理更新依次将Rayleigh红通道乘1.01/1.02/1.03。
unlit使用相同初始相机/几何；动态高度与unlit的配对用于受控增量估计，包含小幅视图差异和串行采样时漂移。

| preset | 高度m | 太阳天顶余弦 | 相机仰角rad |
| --- | --- | --- | --- |
| noon | 1 | 0.7 | 0.18 |
| sunset | 1 | 0.015 | 0.1 |
| high-altitude | 100000 | 0.3 | -0.15 |

地球默认物理参数、单位白色太阳；相机朝向太阳所在方位，FOV=1.15rad、near/far=1/10000m、
曝光8、时间0，两个双面漫反射矩形距相机500m/5000m。完整输入保存于每份JSON的preset，几何构造及源码摘要随manifest保存。
每进程第一张完整天空/几何/AP合成保存为同名PPM；独立天空样例见[大气指南](atmosphere.md)。
这些是可复现的测量预设，不向编辑器添加未实现的预设菜单。

单个预设可这样运行（输出目录需用新名称，手动调用会覆盖同名JSON/PPM）：

```powershell
& ./out/build/windows-graphics/bin/RelWithDebInfo/dk-atmosphere-benchmark.exe `
  --preset sunset --timestamps --output out/atmosphere-sunset/measurement.json
```

`--smoke --validation --timestamps`只检查160×90采集功能，不可纳入1080p性能矩阵。
驱动默认创建新的out/benchmarks/atmosphere-*；可用`--output`指定尚不存在的目录。
失败/超时保留日志及manifest并返回非零，缺失/非有限计时、错误缓存更新、缺失Pass和泄漏拒绝汇总。

## 读报告

manifest保存设备之外的提交/工作区、源码、exe/DLL SHA256和每份原始JSON/PPM摘要；
measurement保存实际设备/驱动、编译器/config、shader指纹、每帧CPU/GPU和VMA数据。
summary按nearest-rank分别计算每轮p50/p95；性能候选判断使用每轮Sky-update大气增量p95≤1.5ms。
每轮失败不可丢弃后重算；三轮中位数不是把120个样本混为一组。

GPU Submission父区间与Pass子区间不要相加。冷编译分为ScenePipeline、AtmosphereRenderer创建及首帧延迟lit/AP编译，
不清除驱动/OS缓存。prepare是同步wall时间，包括LUT提交、等待、读回/数值验证，不是纯CPU计算时间；
record_submit是CPU API wall区间，API端到端包括快照提取到RGBA8 CPU读回，排除文件写出、GUI上传/呈现和IPC。
现有编辑器仍需CPU读回再上传；其额外成本没有用离屏数字替代。

VMA allocation是资源需求，block是分配器保留块；device-local与host-visible可能重叠，不相加。
矩阵结束后另发一个不计时帧，在wait/collect前采样，覆盖AP/HDR临时资源；不会把显存查询开销算进性能样本。
sampled_peak只代表采样点最大值，冷LUT内部临时资源及驱动私有分配不受此值约束；不是系统总显存或精确瞬时峰值。
CPU heap峰值单列，不与VMA相加。质量以线性HDR/CPU参考判断，PPM仅用于查看。
