# 容量与介质吞吐证据

结论见[报告](../2026-10-20-media-capacity.md)；实施和检查结果见[0143](../../development/0143-path-capacity-media-throughput.md)。

- raw：3模式×3轮×新旧两版，共18份Renderer报告；3份默认相机worker性能报告及独立validation报告。JSON采用gzip无损压缩。
- environment.json：机器、命令、实际测量二进制及渲染源码哈希；明确记录基线程序未重新采集exe哈希，以及测量后仅测试/报告字段变化。
- validation：逐次build/CTest摘要、日志和XML，包括失败及修正后的结果，不能把中间失败当通过。
- summary.json：热帧中位数、GPU区间比例、取消/tile p95和默认相机交付间隔。
- manifest.json：除manifest自身以外所有归档文件的SHA-256。

运行`python verify.py`先核对全部哈希，再逐值核对颜色/二阶矩和来源/统计，复算三轮数据。
首帧原始值全部保留；热指标排除每轮第1个采样，以免driver/pipeline预热混入稳态。该比例不能解释为任务管理器占用率。
