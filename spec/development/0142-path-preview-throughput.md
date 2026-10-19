---
id: "0142"
created_at: "2026-10-20T00:00:00+08:00"
updated_at: "2026-10-20T00:19:43+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/render-restir.md
  - ../design/editor.md
  - ../design/graphics-device.md
---

# 0142 默认场景路径预览显存与吞吐优化

## 目标与设计依据

用户在默认 Sponza 的 1600×1200 视口切换 ReSTIR 时触发 1 GiB 额度拒绝，且逐 64×64 tile 提交/等待开销显著。
本次承接 M19.8 的成本归因，按[路径追踪](../design/render-pathtracing.md)、[ReSTIR](../design/render-restir.md)和[Editor](../design/editor.md)设计优化实际预览，不改变分辨率、样本、反弹或估计器。

## 设计与验收

- Renderer 保留默认 1 GiB，允许显式 1 MiB..8 GiB；Editor 按最大 device-local heap 的四分之一及可用预算的一半选择，最高 8 GiB。额度在 worker 创建时冻结，包含外持/在途 owner；实际分配失败仍保留旧图像。
- Surface only 预览采用最大 256×256 tile；Scene media 保留 64，射线/候选工作上界仍可进一步拆分。单批/单票据的取消与完整发布约定不变。
- 无 temporal 复用时，完成后释放仅本 epoch 使用的 surface/reservoir，已完成累积、诊断和来源保持不变。
- Device 支持时启用 VK_EXT_memory_budget/VMA 预算查询，缺少扩展时使用估计；不改变设备准入条件。
- 对照同输入三轮 Surface 四模式 wall/GPU/CPU 提交开销、取消延迟和线性像素；补充 1600×1200 默认场景四模式连续采样、切换与扩展视口验证。
- 定向覆盖 tile 分割等价、显存边界/外持 owner、失败/取消/迟到及 Editor worker/GUI 回归。

## 实际变更

- [Device](../../engine/graphics/device/src/Device.cpp)按能力启用memory-budget扩展与VMA预算标志，缺少扩展不影响设备选择。
- [PathTraceRenderer](../../engine/render/pathtracing/src/PathTraceRenderer.cpp)新增推荐额度、显式上限8 GiB、tile上限256；无temporal时发布前回收96 bytes/pixel临时平面。默认API仍1 GiB/tile64，真实owner计费、单在途/完整批次提交点及失败保护保持。
- [PathPreview](../../engine/editor/src/PathPreview.cpp)采用设备额度与表面tile256，介质tile64；面板显示当前额度。
- [桌面验收](../../tests/integration/PathDesktopWorker.inl)使用真实默认相机和Sponza，1600×1200/2048²四模式连续累积，外持显示图像、取消恢复和关闭检查。
- 扩展progressive/ReSTIR调度测试，逐值比较257×129、64/256 tile与反向遍历下颜色、二阶矩、命中及全部复用组合；更新显存外持回归以维持实际超额拒绝条件。
- 无依赖版本、命令schema、shader或采样算法变化。保留全部24个新旧性能报告、3个桌面性能报告及验证证据，见[测量报告](../benchmarks/2026-10-20-path-preview.md)与[可复算归档](../benchmarks/2026-10-20-path-preview/README.md)。

## 验证记录

- Release：`out/verify/20261020-000351-10afa8f9`，16项通过、0失败/跳过，CTest 96.72秒。覆盖progressive图像/预算/失败/device-lost，ReSTIR初始/时间/空间/介质调度与状态，设备验证、worker、完整桌面分辨率及真实后台GUI/独立捕获。
- 最终将预算下限常量显式写为uint64_t，避免其他平台unsigned long与unsigned long long模板匹配差异；Windows类型/运行行为不变。重建相关程序后 `out/verify/20261020-001430-49e1a6ce` 的预算/桌面两项复验通过，0失败/跳过，14.64秒。
- 两次沙箱内构建分别因vcpkg缓存和MSBuild FileTracker访问权限失败，未执行测试；提权后完整构建通过。失败和成功日志均归档，未用旧二进制冒充通过。
- 三轮同输入640×360表面性能：Classic 42.92→15.50 ms/spp（2.77×），initial 46.74→31.78（1.47×），temporal 77.22→60.45（1.28×），spatial 121.00→62.92（1.92×）。提交数减少90%；32×18线性颜色/二阶矩及逐批来源/统计完全一致。新版tile GPU p95最大7.18ms，取消回收p95最大11.37ms。
- 默认相机1600×1200三轮热交付中位数31/43/232/474ms；2048²分别62/93/521/1080ms。计时包含tone/交接及消费者轮询，不是窗口FPS；详细输入、冷启动与全部轮次见报告。
- 本机RTX5090 D v2/610.88使用5.868 GiB额度，owned峰值2.332 GiB；两个分辨率四模式全部通过。初始RIS完成owner在1600×1200减少175.78 MiB；没有降低尺寸/spp/反弹。
- 离线`verify.py`复算三轮时间、精确比较及归档字节SHA-256通过；文档检查通过11份Markdown、605个本地链接及全局索引/测试入口/JSON，`git diff --check`通过。未跑全工程、Debug、其他设备/无memory-budget扩展硬件或M19.8完整高spp矩阵。

## 遗留问题与下一步

本次两项修复完成。高分辨率temporal/spatial仍昂贵，后续可继续定位其GPU pass内部成本；本次不改变M19.8等误差结论或默认Classic策略。
冷启动编译/导入/AS准备未优化；低显存及外部持有过多图像仍可能正确拒绝。GPU时间/wall比例不是任务管理器利用率，不保证GPU100%。
