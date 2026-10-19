# 路径预览优化证据

[报告](../2026-10-20-path-preview.md)包含测量范围和限制。
raw 保存全部 24 个新旧三轮性能进程与 3 个实际桌面 worker 进程的完整 JSON（gzip）；未删除轮次。
validation 保存16项定向验收、最终2项复验、两次沙箱构建拒绝日志及实际桌面尺寸验收。
environment.json 记录工具链、设备、测量二进制及最终源文件指纹。
summary.json 为聚合结果；`python verify.py` 只读复算四模式时间、计数及精确图像对照，不需要 GPU。
evidence-sha256.json 校验本目录除自身与本说明外的所有证据字节。GPU旧报告中的外部路径不用于本次复算。
