---
id: "0183"
created_at: "2026-11-19T09:55:00+08:00"
updated_at: "2026-11-19T10:19:24+08:00"
status: in-progress
design_refs:
  - ../design/project-foundation.md
---

# 0183 开发工作台与性能证据整理

依据[工程基础设计](../design/project-foundation.md#开发工作台)实现当前工作交接、短报告、CLI 和本地代码索引。
验收覆盖保留的两组离线样本、文档链接、实际脚本调用和两个数据库的源码定位。

## Benchmark 整理与验证

21 份正式报告保留结论、对照、测量条件与复现入口。删除 9 个 ZIP、逐帧数据、重复图像和过时的复算器；最终汇总、独立 UI 验收图和两组离线样本保留。工作树目录由 84,035,666 字节降至约 10.38 MiB。新采集进入 out/，四个既有报告生成器只在 Markdown 中展示最终指标。

- M16.11 的原始 7 个文件完整保留。verify-pathtracing-evidence.py 复现 3 个质量夹具和三轮性能。
- M19.8 原目录在 2026-10-20 已裁为摘要。从 f1e5734 恢复 40 个质量输入后，--verify-quality 重算全部十场景四模式并匹配原汇总。历史完整归档在 out/workflow-0183/historical 恢复并通过 --verify-archive，包括所有图像和哈希。首次复算暴露旧 sampler 2 与当前 sampler 3 不同，校验器按冻结 manifest 选择相应协议。
- 光栅与联合体素的三轮真实存量输入重新生成短表，模拟的原三轮样本重新生成六行 OFF 对照。完整 Memory 采集和 GPU 性能矩阵未重跑。
