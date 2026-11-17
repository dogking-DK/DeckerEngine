# 光追大气与云：保守密度查询优化（2026-11-17）

对应[0180开发记录](../development/0180-path-medium-majorants.md)。默认启用局部上界粗筛、天气/高度轮廓及base零密度剔除，保留model4/sampler2、原连续云场、全局追踪随机序列和物理多次散射。分段局部majorant已实现并通过数值验收，但实测增加耗时，保留原生实验开关、默认关闭。

## 测量范围

NVIDIA RTX 4070 Laptop / 8188 MiB / 驱动596.49，MSVC19.51.36260.0。正式数据来自`windows-voxel-editor`的Release构建，OpenVDB/NanoVDB/原生体素路径开启。Sponza固定相机、时间17秒、seed101、4次反弹/RR3；640×360，每批1 spp，tile256、每提交4 tile（每批共6 tile/2提交）。原生GPU timestamps ON，validation/Tracy OFF，33个独立进程串行运行，三轮轮换开关顺序。

每进程先预热256批，再记录20批。预热循环0..15样本区间，测量独立重置后使用0..19；不删弃测量中的慢帧。表中是三轮各自p50的中位数。GPU数值覆盖路径积分、累积和批次诊断读回；完整图像CPU读回、色调映射、GUI显示不在该计时中。它不是1080p光栅云的整帧耗时，也不能直接换算为最终收敛画面的FPS。

实现前先冻结109559f旧二进制、源文件与输入并完成三轮低太阳基线，GPU p50为8.433/8.446/11.758 ms/spp。短预热有明显频率波动，所以这些数据仅证明改动前的真实基线，不能与最终更长预热、不同构建选项直接求收益比。正式A/B在同一最终二进制中关闭/开启优化；旧冻结基线与关闭优化模式的32×18同样本图像/二阶矩一致。

## 性能结果

| 场景 | 原算法 GPU ms/spp | 默认优化 GPU ms/spp | GPU耗时变化 | 原/新 wall ms/spp |
|---|---:|---:|---:|---:|
| 日落 | 4.992 | 4.429 | -11.28% | 6.110 / 5.615 |
| 无云空气（控制组） | 3.012 | 3.273 | +8.65% | 4.120 / 4.122 |
| 厚云 | 4.347 | 4.447 | +2.30% | 5.436 / 5.539 |
| 云内 | 8.754 | 6.789 | -22.44% | 10.022 / 7.880 |
| 纯mesh（控制组） | 2.383 | 2.389 | +0.24% | 3.428 / 3.380 |

日落三轮原算法4.954/4.992/4.992ms，默认优化4.429/4.428/4.515ms，每轮改善；中位耗时减少11.28%，wall减少8.09%。云内GPU减少22.44%。厚云增加0.100ms（2.30%），不能声称所有天气都更快。控制组使用同一原程序，GPU时间仍有设备调度/频率波动，无云空气wall基本不变。

日落分段实验为6.434/6.388/6.376ms，中位6.388ms，比原算法慢27.96%，比默认查询优化慢44.24%，因此不启用为默认。

无云空气和纯mesh使用原程序，不声称算法收益。分段局部追踪通过8 seed数值检查，但没有通过默认启用的性能门槛；局部上界已用于默认查询粗筛，不需要为此改变候选位置。实验分段追踪包含天气空格跳过、空气解析上界、粗格穿越计费、短光学区间及大位移全局回退。

## 占用与原因

日落默认策略的路径积分zone占批次GPU时间约99.86%。20 spp累计追踪工作原/新均为14,291,201；分段实验为15,645,466（含区间退出），额外遍历未换来净收益。

640×360第20批的引擎自有Vulkan分配原/新为467.712/467.713 MiB，差1024字节来自额外变体资源；同一A/B二进制的两侧都上传4608字节上界。整个测试进程的自有分配峰值原/新为875.021/875.022 MiB。该统计不包含驱动私有管线缓存。

冷成本单列：正式矩阵首次base程序创建16.772秒；默认变体首次预热批（含变体编译、管线创建及一批渲染）17.424秒，后两轮进程为3.267/3.387秒。renderer内后续批次复用变体；本次没有声称冷启动更快。

默认128² RG8 weather、128³ R8 base使用edge=8的粗格，附加512+4096=4608字节上界，和原场一起上传、计入同一不可变owner及预算；detail侵蚀非负，上界不需要额外detail网格。最大值覆盖完整插值texel支撑与repeat halo，不使用稀疏点采样估计。时间、太阳、覆盖率或密度改变复用场；更换seed/分辨率才重新上传。无有效云时直接复用原程序。

计数是已完成GPU路径的追踪工作，包括实验路径的区间退出；它不是显存读取次数或GPU占用率。收益来自少做连续场插值，未降低分辨率、反弹、样本数或太阳采样精度。当前GPU zone覆盖整个路径积分，不能据此把Rayleigh/Mie/HG、场查询分别精确归因；本次未采集Tracy/SASS/硬件占用率。独立着色器变体避免动态算法分支造成的开销，但没有把预跑变慢直接当作已证明的寄存器溢出。

## 画质与正确性

- 默认策略与关闭优化的日落640×360、20 spp线性图像，以及五场景32×18、4 spp线性均值/二阶矩，最大绝对差均为0（门槛1e-6）；追踪计数相同。
- CPU粗格最大值包含性通过312,513个断言，包含负周期、格边界和halo。
- 实验分段追踪使用独立确定性透射及CPU全局追踪参考，8 seed，N=16,384、4N=65,536；低太阳混合空气/云的参考N/4N集合nRMSE为0.3422%（门槛0.5%），GPU/CPU集合nRMSE为0.0671%（门槛2%），各通道同时满足2%与5倍联合标准误差+1e-4；保留真实多次散射。
- 同策略batch/tile、缓存复用/更换、旧owner、迟到/取消、追踪额度、上传失败保护，以及mesh/voxel混合、透视/正交、Classic/四DI、capture和后台GUI通过。CPU参考不使用新GPU上界。

完整Sponza 4×4、8独立seed、16,384/65,536 spp参考长链实际运行完成，required validation通过；参考N/4N集合nRMSE为2.0894%，未满足0.5%。候选16,384 spp相对该参考的多链nRMSE为4.3195%（上/下ROI 4.2610%/5.2063%），集合均值nRMSE为1.9177%。所以`quality_spp`与`quality_time_ms`均为null；不放宽2%画质门槛、不把未收敛参考当作画质认证。默认优化的保持画质证据是同样本逐值一致和独立数值回归。

## 已知限制与失败记录

- 128 spp长链在冻结旧二进制/旧着色器与新关闭优化模式都触发既有`GPU medium path exceeds numerical domain`。本次没有把该范围写为通过，也没有修改物理系数或数值域保护来掩盖失败；正式吞吐维持20 spp范围。该问题需要独立诊断。
- 最初单线程一次65,536样本的长水平分段追踪触发device lost；测试改为256样本一批，连续全局样本编号和总样本数不变，之后通过。未修改TDR或驱动配置。
- 一次required检查被AMD switchable隐式层的API版本警告拒绝；随后仅对子进程设置该层支持的禁用变量，保留Khronos/sync验证，最终零validation错误/警告。
- 分段追踪仍为实验选项，不开放为GUI默认质量档；新的着色器变体首次使用有编译/管线创建成本，缓存后的稳态收益不代表冷启动收益。

## 复现与证据

```powershell
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1='1'
python scripts/benchmark-path-medium-bounds.py --binary out/build/windows-voxel-editor/bin/Release/dk-pathmedia-benchmark.exe --output out/benchmarks/path-medium-repeat
python scripts/benchmark-path-medium-bounds.py --analyze --output out/benchmarks/path-medium-repeat
& out/build/windows-voxel-editor/bin/Release/dk-pathmedia-benchmark.exe --root projects/demo --case low-sun --algorithm classic --tile 256 --tiles-per-submission 4 --quality --output out/benchmarks/path-medium-repeat/quality-low-sun.json
python scripts/benchmark-path-medium-bounds.py --quality-input out/benchmarks/path-medium-repeat/quality-low-sun.json --output out/benchmarks/path-medium-repeat
```

原生消融：`--no-density-culling --no-local-majorants`是原算法；默认启用严格查询剔除；`--local-majorants`启用分段实验。它们是benchmark/C++ API选项，不是Editor Console命令。

结构化结果见[JSON报告](2026-11-17-path-medium-optimization.json)，原始输入、逐tile时间、完整同样本线性图像、冻结源码/二进制SHA、质量长链、定向验证和失败记录见[证据包](2026-11-17-path-medium-optimization.raw.zip)。证据保留精确测量源码字节；Git会规范化CRLF。解包后把`matrix`目录传给`--analyze`可复算正式计时与同样本检查。
