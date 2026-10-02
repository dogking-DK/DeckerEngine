---
id: "0052"
created_at: "2026-10-02T20:58:00+08:00"
updated_at: "2026-10-02T21:07:00+08:00"
status: completed
design_refs:
  - ../design/graphics-vulkan.md
  - ../design/graphics-resources.md
---

# 0052 M5.5.4 批量上传与异步读回

## 范围与提交点

将 upload/readback 便利方法直接放在 CommandBatch，复用已有 prepare/copy 及同一提交单位，
无需第二层 TransferBatch 借用对象。每次独立 staging，多次操作只在调用者 submit 时提交。
ReadbackRequest 的完成记录预先分配，submit 成功时绑定 pending；失败/放弃标记 cancelled。
pending slot 保留 staging 与记录，request 不强持有 queue，queue 排空后可继续读取。

## 验证

Transfer.hpp/Transfer.cpp 提供 buffer slice、带 pitch 的 color upload、紧密排列区域 image readback，以及请求状态/try_read。
ResourceInternal/Resources 预先分配请求列表，成功提交后无分配转移；正常 queue 析构等待后发布 ready，设备丢失不伪造成功。
组合录制在首条命令之后发生失败时使批次 invalid；输入校验在录制前完成。

验证使用 windows-graphics Debug，目标 dk_graphics_resource_tests、dk_offscreen_probe、dk_graphics_resource_probe，
筛选 `^dk\.(graphics\.unit\.|graphics\.gpu_resources_validation$|offscreen\.gpu_validation$)`。
执行 scripts/verify.ps1，Reason 为 `M5.5.4 batched staging and independent readback lifecycle`。
结果：out/verify/20261002-210410-63308509，9/9 通过、零跳过，两个 GPU probe 均零警告/错误、Memory/VMA 无残留。
沿用进程级 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，结束恢复；Khronos 同步验证开启。

新增资源 probe 覆盖多上传/读回仅一次提交、buffer 分段、带 pitch 的 mip/layer、color 子区域、
未提交/超时不写输出、丢弃请求、批次取消/提交失败、queue 销毁后读取、设备丢失终态；
关闭 CPU heap 后提交与请求转移仍成功，验证提交点无分配。
文档检查与 git diff --check 通过。未运行全量/Release/其他平台；深度 byte readback 和 staging 池仍暂缓。
下一步 M5.5.5 迁移 Offscreen 与使用指南，复验 CPU runner 和独立 shader 配置。
