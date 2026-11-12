---
id: "0165"
created_at: "2026-11-12T10:40:16+08:00"
updated_at: "2026-11-12T11:32:53+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
---

# 0165 M23.7 光栅稀疏基线与 HDDA 调优

## 目标与设计依据

执行 [M23.7](../roadmap.md#m23实体体素渲染) 和[基线实施契约](../design/render-voxels.md#m237-光栅基线实施契约)。
建立质量/转换/上传/帧耗时/内存/在途回收的可复算基线，保留原生路径后续阶段。

## 实际变更

- `dk_voxel_benchmark`：12 个版本化输入、两种投影/分辨率，独立 DenseGrid/解析盒体参考、GPU 时间戳、VMA 与 Windows 进程内存采样。
- HDDA 单轴转换省去重复邻格读取，边角保留原语义；旧转换及逐格步进 shader 对照，不改变公共 API 或缓存所有权。
- `scripts/benchmark-voxels.py`：三轮串行 Release 与严格聚合，输出原始 JSON、摘要和表格；缺失/混合输入、错误计时、质量/回收失败拒绝。
- 无损 gzip 原始数据、来源清单、图表和[基线报告](../benchmarks/2026-11-12-voxel-raster.md)纳入版本管理；第一候选和未做设备预热的结果单独归档。

## 验证记录

- `20261112-104455-98e231fb`：Slang、SPIR-V、真实 GPU numeric 3 项通过；基准因诊断读回上限失败。
- 保留生产深度读回 65536 像素额度；改为全尺寸颜色 + 同投影/宽高比的 256 宽深度诊断帧。曾遇 Windows `small` 宏重名，已改名。
- `20261112-105019-79c0be38`：6 输入 required GPU smoke 与汇总校验通过；0 错误/警告及泄漏。
- `20261112-105251-7842265e`：独立无 RT 光栅配置 Release 构建及聚合检查通过；该配置需显式 `find_package(nlohmann_json)`，不新增/升级依赖。
- `20261112-105349-269189ac`：初候选 6 GPU + 1 聚合检查通过，覆盖资产/Editor/capture、完整逐像素 oracle、合成及寿命。
- 完整矩阵首次在 checker64 的正交诊断帧失败：反投影将精确平行格面舍入到负侧相邻单元。CPU 参考按既有正侧单元规则，仅修正 double 矩阵舍入并记录数量；不剔除像素，不改变颜色/深度门槛。修正后完整三轮通过。
- 第一候选的跨格前判断使多岛查询旧/新比为 0.967，去掉该额外判断，仅保留单轴命中省查值。
- `20261112-110854-96999967`：最终 HDDA 在 Release 的 9 项检查全通过（Slang/SPIR-V、benchmark、numeric/offscreen/lifetime、raster composition/lifetime、聚合）。
- `20261112-111044-f4c2ef15`：最终共享 HDDA 的 Editor/Console/IPC/Gizmo/独立 capture 再验通过。
- `20261112-112141-dedaab96`：加入固定设备预热协议后的 Release required GPU 基准及聚合检查通过。上述最终范围共 10 个不同 CTests：3 CPU、7 GPU，无跳过；全部 required validation/sync 零错误/警告，CPU/GPU 回收计数为零。
- 正式协议 v2 完整三轮：`out/benchmarks/20261112-voxels-v2`，36 个输入执行、144 个视图配置、1152 个正式采样帧，三轮结果摘要一致。设备预热额外 64 帧/轮及每视图首帧/预热全部另行保留。
- 最终报告 GPU submission 中位数 0.212–1.127 ms、端到端 1.883–6.907 ms；单轴候选旧/新比 0.996–1.010，不宣称可靠提速。保活 frame allocation 最大 90.87 MiB，内部 scratch 精确峰值未认证。

可复算的主要命令：

```powershell
$env:VCPKG_ROOT = 'C:\code\third_lib\vcpkg'
$env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
./scripts/verify.ps1 -BuildDir out/build/windows-voxels-raster -Configuration Release -Target dk_voxel_benchmark,dk_voxel_gpu_probe,dk_voxel_raster_probe -TestRegex '^dk\.voxels\.(benchmark_(gpu_required|aggregation)|slang_compute|spirv_validation|gpu_(numeric|offscreen|lifetime)_required|raster_(composition|lifetime)_required)$' -Reason 'M23.7 final HDDA acceptance' -Parallel 4
python scripts/benchmark-voxels.py --exe out/build/windows-voxels-raster/bin/Release/dk-voxel-benchmark.exe --output out/benchmarks/voxels
```

归档 gzip 数据独立复算通过，摘要与提交报告一致；来源 SHA-256、两份 Python AST 及 `git diff --check` 通过。
聚合测试含 3 个测试方法（完整、预热保全、16 种坏证据注入）全部通过；`check-spec.ps1` 验证 11 份 Markdown、912 条本地链接及元数据/索引/测试入口/JSON。
图表使用 matplotlib 生成并人工查看。各轮准确命令、日志与早期失败保留在 `out/verify`。

## 遗留问题与下一步

M23.7 完成；下一阶段为 M23.8 原生 intersection/Slang 门禁，M23 父阶段不关闭。
初版两帧视图预热不足以隔离首输入的跨轮慢态，采用协议 v2 固定设备预热并重采；未锁频，未确定启动慢态的硬件根因。
采样峰值包含 OpenVDB/TBB、分配器/驱动及前序用例保留，不是独立 scratch 上限；VMA 也不覆盖全部驱动显存。
未运行跨厂商/跨平台/更大视口、文件解码吞吐或原生路径性能；不将离屏图像提交时间当作 Editor FPS。

## 修改记录

- 2026-11-12T10:40:16+08:00：固定矩阵、计量口径与质量门槛。
- 2026-11-12T11:25:00+08:00：完成最终候选、required GPU 回归、三轮协议 v2 与原始数据归档。
