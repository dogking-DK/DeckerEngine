# 体积云质量与性能复现

固定协议见[云设计](../design/render-clouds.md#m157-质量与性能采集协议)，本机结果见[联合交付报告](../benchmarks/2026-10-13-clouds.md)。
质量使用 required GPU/synchronization validation；性能使用独立优化构建，保持同一物理配置与生产质量。
下列命令从仓库根执行，Python 3.11+ 只需标准库。同一构建目录的配置、构建、CTest 串行执行。

## 固定质量集与回归

```powershell
cmake --preset windows-editor -DDK_BUILD_SCRIPTING_LUAU=ON -DDK_ENABLE_PROFILING=OFF
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor `
  -Target @('dk_cloud_benchmark','dk_cloud_reference_tests','dk_cloud_composition_probe','dk_run','dk_ctl','dk_editor_app') `
  -TestRegex '^dk\.(clouds\.(optimization |benchmark_aggregation$|benchmark_gpu_validation$|quality_.*_gpu_validation$|(optimization|optimization_boundaries|temporal|history|shadow_numeric|shadow_surface|shadow_failures|failures)_gpu_composition$|sequence_gpu_validation$)|editor\.cloud_consistency_gpu_validation$)' `
  -Reason '四预设质量、动态历史、云影、失败关闭及脚本GUI一致性'
```

需要真实 GPU 和 Khronos validation，不允许 skip 替代验收。GUI 自动化使用 `--background`。
本机 AMD switchable 隐式层冲突时，对当前命令临时设置 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`，用 try/finally 恢复；不关闭 Khronos 层。
每次 CTest 后及时复制 `out/build/windows-editor/Testing/Temporary/LastTest.log` 到对应 `out/verify/<run>/details.log`，保留完整数值。

探针的 `--quality --validation --preset sunset --output <new-path>.json` 可单独复现一个预设；直接调用会覆盖同名文件。
四种预设共 58 个静态/运动/云内/云上参考比较，31² 线性图，静态 32 帧预热后测量 120 帧相邻差，连续运动 12 帧。
参考为 512/128/128 和 1024/256/256 主/太阳/空气步；高档裁剪投影为至多 16² 四块，保持原像素中心，另验证同档分块等价。
普通像素相对参考亮度 p95 归一；太阳盘按视线和角半径分类并用盘辐亮度归一，独立检查透射误差，未掩码值仍保存。
逐帧云 Tc、太阳、全图与非太阳最大值单列；不以平均误差掩盖边缘，不把 31² 质量结果外推为 1080p 每像素保证。
旧探针补充独立 CPU/解析解、反遮挡、云影世界稳定性以及失败/关闭/在途寿命。

## 三轮 1080p 矩阵

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo `
  -Target dk_cloud_benchmark -TestRegex '^dk\.clouds\.(benchmark_aggregation|benchmark_gpu_validation)$' `
  -Reason '优化版云性能探针和聚合契约'
python scripts/benchmark-clouds.py --build-dir out/build/windows-editor
```

运行期间保持设备空闲，不执行其他 GPU 测试或修改相关源码/二进制。驱动检查源码、exe/DLL 与 shader 摘要未变化。
默认创建新的 `out/benchmarks/clouds-*`；`--output` 只接受尚不存在的目录。失败/超时保留 manifest、原始数据和日志，返回非零，不自动重试或丢弃轮次。
性能关闭 validation 和 Tracy 编译，独立 GPU timestamp OFF/ON 配对；这不是 Tracy 轨迹采集。

24 个独立进程 = 3 轮 × clear/sparse/overcast/sunset × timestamp OFF/ON；轮换预设和开关顺序。
每进程依次采集 combined-static、combined-motion、air-static、air-motion、unlit-static、unlit-motion：
每段首帧单列，combined 预热 32 帧、其他段 5 帧，各取 20 稳态帧，共 210 个计时帧；每段另有两个不计时显存诊断帧。
所有 motion 段同索引使用相同 x=0.25m/步、yaw=0.0005rad/步、time=1/30s/步；每段先释放旧视图历史。
时间变化重建云影，相机位置变化重建 Sky；静态命中缓存不会重复加入旧 Submission 耗时。

输入复用公开 `environment_preset()` v1，和编辑器/命令共享完整物理预设，固定 seed=1729、generator_version=1、噪声 128/128/32。
质量为 cloud_temporal_v1：半尺寸、96–256 主步、8 太阳步，低太阳角度重建最低 32 步，默认空区跳跃/早停/历史；MS 开启。
云影 center=0、extent=8192m、256²/256 步；相机 [0,100,0]、仰角 .16rad、FOV=1.15rad、near/far=1/10000m、曝光 8、时间 0。
两个固定世界双面接收体距离 500m/3000m，后者 checker alpha-mask。完整物理输入、路径、shader 指纹和源码摘要随 JSON 保存。
每进程最后一个 combined-static 帧保存同名 PPM；三轮和 timestamp 开关的同预设图像必须逐字节一致。

## 指标与边界

GPU 总量是本帧更新的 LUT、云影及完整帧 Submission 的不重叠时长之和，Pass 子区间只作归因。
云增量=combined−同索引 air，联合增量=combined−同索引 unlit；保留负差，不相加独立 Pass p95。
先每轮按 nearest-rank 计算 p50/p95，再报告三轮中位数和范围。20 样本的 p95 是有限样本估计。
候选云增量 4ms、联合 5.5ms 按 motion 每轮 p95 判断；clear 是零云控制组。

CPU 分列快照提取、同步 prepare、record/submit、wait、RGBA8 读回与 API 端到端 wall。
初始化分列设备/队列、程序与几何、CPU 云场、纹理上传 wall；纹理上传 GPU 时长未采集，字段为 null。
不清空 OS/驱动缓存；同步 prepare 含 GPU 等待，不是纯 CPU 算法耗时。
端到端不含 GUI 上传/呈现、IPC 或文件发布。GPU 频率/功耗未锁定，固定 mode 顺序与串行基线存在时序漂移。

显存诊断先保留已完成外部 Frame，再提交下一帧；wait/collect 前采 VMA，之后释放 Frame、poll 历史并采稳态缓存。
环境资源增量扣除程序/几何基线；128MiB 稳态、192MiB 在途采样峰值是候选。
探针预先生成所有预设的云场和纹理，包括 clear 控制组，相关成本明确留在初始化和缓存统计。
device-local/host-visible 类型可能重叠，不相加；allocation 与 block 分列。CPU heap 不是进程 RSS。
采样峰值不包含驱动私有内存，也不保证覆盖冷初始化内部所有瞬时峰值；所有引擎资源释放后检查 VMA 回到设备初值、CPU 活分配为零。
无效计时、缺帧、重复区间、错误缓存/历史、配置变化或关闭泄漏都拒绝汇总；候选目标未达与采集失败分别报告。
