# 路径追踪质量与性能基准

入口：[设计协议](../design/render-pathtracing.md#m1611-质量与性能交付协议)、[实测报告](../benchmarks/2026-10-15-pathtracing.md)、[开发记录](../development/0120-pathtracing-delivery.md)。
先按[构建指南](build.md)配置包含 Pathtracing、Render Disk、tests 的 GPU 构建；需要真实 RT、sampled-image 非一致索引、Khronos validation 和 Python 3.11+。
`dk_pathtracing_benchmark` target生成 `dk-pathtracing-benchmark` 离屏专用探针，不启动 Editor，也不修改磁盘夹具。

## 质量与拒绝检查

```powershell
$env:VCPKG_ROOT='C:/code/third_lib/vcpkg' # 使用本机实际工具链目录
& ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Configuration RelWithDebInfo `
  -Target dk_pathtracing_benchmark -TestRegex '^dk\.pathtracing\.benchmark_' `
  -Reason '固定质量、参考收敛及原始报告完整性'
```

质量使用 required validation/synchronization validation；缺少设备或层即失败，不以跳过代替。
三个夹具各8 seed、4×4线性RGB，CPU N/4N=16384/65536；同时验证GPU无历史65536 spp、渐进64/256/1024 spp及动态重置。
测试输出在构建目录的 `test-artifacts/RelWithDebInfo/path-benchmark-quality/<run>/`。
聚合测试包含无GPU的门槛拒绝、已归档三轮数据重算和损坏/缺失文件拒绝。
独立CPU解析/能量参考使用 `dk_pathtracing_reference_tests`，选择表见[测试入口](../../.agents/skills/decker-build-verify/references/test-selection.md)。

## 完整三轮性能

```powershell
$env:VK_LAYER_VALIDATE_SYNC='1'
$env:VK_VALIDATION_VALIDATE_SYNC='1'
python -X utf8 scripts/benchmark-pathtracing.py `
  --executable out/build/windows-editor/bin/RelWithDebInfo/dk-pathtracing-benchmark.exe `
  --output out/benchmarks/pathtracing
```

脚本先运行质量进程，再串行启动三个独立性能进程。性能进程显式关闭 validation、开启 GPU timestamps；上述同步层环境变量不会主动启用层。
本机 AMD 隐式层冲突时只在测试子 shell 设置 `DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`；其他机器不照搬此设备规避项。
每轮固定1080p、8反弹、1 spp/批、64像素tile；首批单列，再测六批稳态，不自动降低负载。
另测4×4质量档位的实际累计时间、tone、全图线性读回、在途取消/回收及显存。
避免与其他GPU工作并行；当前基线未锁定频率/功耗，不声称时间波动来自单一原因。

`--quality-only` 仅执行质量；`--quality-report <run>/quality.json` 可复用本机已经通过的质量数据，
但必须有对应 manifest，且二进制/源码/夹具/报告SHA256全部一致。
`--smoke` 将三轮性能负载改为160×90、两批，用于工具调试，不能作为1080p交付证据。
每次创建独立目录，不覆盖旧报告；保留 stdout/stderr、raw JSON、manifest 和 summary。
运行失败保留原因和已有文件，禁止手改状态、删除慢轮或调低质量门槛后宣称通过。

## 无GPU重算归档

```powershell
python -X utf8 scripts/verify-pathtracing-evidence.py spec/benchmarks/2026-10-15-pathtracing
```

校验压缩文件及解压后的SHA256、轮次完整性、输入一致性，重新计算质量/性能指标并与 summary 比较。
原始JSON以gzip保留字节，避免Git换行转换改变测量摘要；manifest/summary本身使用LF。
这是既有证据的复核，不能替代当前硬件上的GPU运行。二进制只记录SHA256，没有将可执行文件纳入仓库。

时间解释：每批GPU总量为各tile提交区间之和，含该提交的trace/accumulate和状态读回；父子zone不重复累加。
trace与accumulate共用shader，不能伪造拆分值。wall包含工作线程提交、等待、poll及采集开销，
不包含GUI呈现、IPC或文件发布；显式全图读回另列。4×4/8 seed达到2%质量的时间不能外推为1080p收敛时间。
