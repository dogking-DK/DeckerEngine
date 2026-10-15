# M16.11 路径追踪质量与性能交付（2026-10-15）

三个固定输入的CPU参考收敛、CPU/GPU线性对照、多seed渐进趋势和动态恢复通过；三轮独立性能数据已归档。
**1080p每批1 spp的端到端16.67ms候选均未达到。GGX盒在1024 spp仍未达到2%质量候选，保留未达状态。**
默认1GiB资源额度内完成全部负载，退出时CPU/VMA存活分配均为零。

[复现指南](../guides/pathtracing-benchmark.md)、[设计协议](../design/render-pathtracing.md#m1611-质量与性能交付协议)、[开发记录](../development/0120-pathtracing-delivery.md)。
归档：[manifest](2026-10-15-pathtracing/manifest.json)、[summary](2026-10-15-pathtracing/summary.json)、[完整性索引](2026-10-15-pathtracing/archive.json)、
[质量原始数据](2026-10-15-pathtracing/quality.json.gz)、[第1轮](2026-10-15-pathtracing/round-1.json.gz)、[第2轮](2026-10-15-pathtracing/round-2.json.gz)、[第3轮](2026-10-15-pathtracing/round-3.json.gz)。
解压后保留逐seed线性RGB和逐tile CPU/GPU测量，没有只保留汇总统计。

## 环境与范围

Windows NT 10.0.26200，AMD Ryzen 9 7940HX，32逻辑处理器；NVIDIA GeForce RTX 4070 Laptop GPU，驱动596.49。
MSVC 19.51.36260.0 / RelWithDebInfo，Slang 2026.18；Tracy编译/连接均关闭。
父提交48364616ca908d3081c15d8e90eea622eaf77026，实际被测工作区的源码/夹具及二进制SHA256见manifest。
性能validation OFF / timestamp ON，period=1ns、validBits=64；质量required Khronos及同步验证。
仅子shell禁用冲突AMD switchable隐式层，没有修改系统环境或升级依赖。

输入是4三角形Lambert地面/面积灯、12三角形GGX盒、6三角形/1纹理的磁盘render-disk mask场景。
固定场景身份/相机/环境/模型版本和输入指纹均在raw JSON。程序化灯位于相机后方，照亮地面；本组没有直接可见的高亮灯像素，bright ROI为null。
上/下半图ROI仍单列；小亮灯、掠射、mask阈值、解析白炉与独立积分另由已有定向用例覆盖，不声称4×4图覆盖任意空间细节。
不是Sponza或生产场景，也未测其他设备/驱动、介质积分、去噪或完整PBR。

## 固定质量

4×4、4反弹、RR=3，seed为101+7919*i（i=0..7），环境RGB=(.15,.2,.3)，无太阳，MIS。
CPU N/4N=16384/65536，按8seed平均图检查参考收敛≤0.5%；同seed 65536 spp CPU/GPU合并nRMSE≤2%。
渐进误差为各seed图对CPU 4N平均图的合并nRMSE，要求集合趋势下降，未要求每像素/seed单调。
下表均为线性RGB归一误差百分比。

| 输入 | CPU N/4N | CPU/GPU | 64 spp | 256 spp | 1024 spp | 2%候选 |
| --- | --- | --- | --- | --- | --- | --- |
| area-lambert | 0.11006 | 0.0001821 | 6.2084 | 3.2218 | 1.4511 | 1024 spp达到 |
| ggx-box | 0.16926 | 0.0005156 | 8.6038 | 4.8883 | 2.2824 | 本采样阶梯未达 |
| disk-mask | 0.02470 | 0.0000037 | 1.6874 | 0.6796 | 0.3679 | 64 spp达到 |

GPU相对CPU的全体RGB均值偏差分别为−0.00005114%、+0.00025677%、+0.00000343%；最大绝对通道差分别为1.24e-6、3.12e-6、1.19e-8，均值与空间误差同时保留。
按seed平均RGB均值计算的95% t置信区间分别为0.15837494±0.00004926、0.14540449±0.00007639、0.17169062±0.00002067；
像素不作为独立样本。三种输入的reset、相机变化、pending取消恢复均与新视图逐像素一致（绝对容差1e-6）。
没有降低2%候选或提高0.5%参考收敛门槛来消除未达项。

关闭validation的实际质量时间按同一4×4/8seed阶梯采集，包含每档图像读回和之前档位的采样；仅从64/256/1024中选首个达到档位。
Lambert达到1024 spp的8seed总wall为287.514 / 286.213 / 281.324ms；磁盘达到64 spp为11.046 / 11.365 / 11.166ms。
GGX三个进程均未达，达到耗时为null，没有外推采样数或1080p时间。

## 1080p时间基线

1920×1080、8反弹、RR=3、seed=101、1 spp/批、64×64 tile，每批510次提交。
每轮新进程：首批单列，后续六批各自取p50/p95；p95采用nearest rank，六批时等于最大值。
不混池、不删慢样本；进程冷启动不等于清空驱动/OS磁盘缓存。未锁GPU功耗/频率，timestamp采集和JSON组装开销未做OFF对照。
以下单位ms；wall属于独占队列benchmark worker，含逐tile提交/等待/poll和采集，不是GUI owner响应延迟。

| 输入 | GPU p95第1/2/3轮 | wall p95第1/2/3轮 | 三轮GPU p50中位数 | 三轮wall p50中位数 |
| --- | --- | --- | --- | --- |
| area-lambert | 22.301 / 22.387 / 22.226 | 133.684 / 134.098 / 136.228 | 22.281 | 131.958 |
| ggx-box | 33.875 / 33.857 / 33.739 | 140.301 / 143.252 / 143.982 | 33.773 | 140.246 |
| disk-mask | 10.764 / 11.059 / 10.849 | 115.723 / 122.661 / 118.276 | 10.746 | 114.073 |

GPU总量是510个提交区间相加，不含提交间空闲；wall为完整批次。仅disk-mask的GPU区间低于16.67ms，三例wall均未达到候选。
首批wall第1/2/3轮分别为Lambert 133.210 / 132.619 / 134.099、GGX 141.434 / 142.341 / 137.637、disk 109.292 / 108.471 / 110.086。
冷设备/队列为433.808 / 395.041 / 392.115ms，编译/管线为367.743 / 363.303 / 370.472ms。

下表为三轮各自稳态中位数的中位数；子区间仅用于归因，不与父区间相加。

| 输入 | GPU trace+accumulate | GPU状态读回 | CPU record+submit | CPU wait+poll | GPU tone / wall | GPU全图读回 / wall |
| --- | --- | --- | --- | --- | --- | --- |
| area-lambert | 20.204 | 1.358 | 66.557 | 60.645 | 0.305 / 0.624 | 20.213 / 198.979 |
| ggx-box | 32.022 | 1.155 | 67.448 | 67.767 | 0.348 / 0.577 | 20.221 / 189.969 |
| disk-mask | 9.009 | 1.144 | 65.577 | 43.672 | 0.351 / 0.669 | 20.214 / 185.547 |

trace和accumulate在同shader，表中联合计时；每tile名为HDR readback的zone只复制状态字，不代表全图HDR下载。
全图读回另显式执行，wall含缓冲准备、等待和CPU线性图构造；tone/readback每轮各一次，因此取三轮中位数，没有六帧p95。
不含窗口呈现、IPC、文件编码/发布；GUI owner由后台真实交互验收覆盖，未给出其延迟分位数。

CPU输入准备三轮中位数为2.070 / 1.367 / 5.109ms，上传+AS的wall为2.539 / 0.464 / 0.413ms。
对应GPU upload/BLAS/TLAS为Lambert 0.0030/0.0763/0.0379、GGX 0.0023/0.0864/0.0379、disk 0.0017/0.0531/0.0251ms。
每输入仅首次完整BUILD，不包含refit；这些时间不能代表大型AS性能。

## 资源、取消与退出

三轮相同：完成图像稳态约253.131MiB；旧完成图保活且下一批在途时约506.256MiB。
renderer累计owner峰值506.258MiB，小于1024MiB额度；VMA设备block峰值602.25MiB，区别于实际owner字节，不代表整机显存总占用。
峰值随renderer累计，disk的peak继承之前GGX略高值；释放view后约0.006–0.008MiB仅保留场景，renderer/queue关闭后VMA分配数为0。
CPU被路由heap峰值约649MiB（包括线性全图读回），不是进程全部RSS；退出live allocations为0。

取消调用0.0002–0.0005ms，受计时精度影响，不作为硬实时承诺；最终等待/丢弃/回收为22.314–26.536ms。
不发布pending样本，completed_spp保持7，随后可正常释放与重用。取消只表示接受逻辑取消，不能抢占GPU已提交工作。
每轮零诊断错误/警告。正确性另外使用validation；关闭验证层的“零诊断”不能替代同步验收。

## 验收与后续

CPU-only解析/独立积分/纹理/输入契约26项、required GPU数值/渐进/失败/额度及实际GUI/capture22项、新质量与报告校验2项通过，均无跳过。
GUI仍使用--background，验证14个检查点及三组GUI/capture线性一致；本轮只对路径追踪直接链路回归，没有全引擎或Debug/Release双配置测试。
归档可无GPU重算并逐字节校验；原运行目录out/benchmarks/pathtracing/20261015-113919-a114dd0c。
后续优化优先考察510次串行提交/等待、GPU路径成本、累积内存和显式全图读回；质量门槛保持。
GGX达到2%所需更高spp的测量、1080p质量分布、跨设备表现与GUI延迟量化仍未完成，不据此承诺60fps。
