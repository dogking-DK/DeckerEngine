---
id: "0143"
created_at: "2026-10-20T00:31:00+08:00"
updated_at: "2026-10-20T01:19:00+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/render-restir.md
  - ../design/editor.md
  - ../design/render-disk.md
  - ../design/assets-importers.md
  - ../design/render-capture.md
---

# 0143 路径预览按设备容量准入与介质吞吐

## 目标与设计依据

承接0142，用户要求默认取消人为容量上限，仅以可用显存准入；Scene media仍显示低GPU占用，并报整帧ray/work预算错误。
按[路径追踪](../design/render-pathtracing.md)、[ReSTIR](../design/render-restir.md)和[Editor](../design/editor.md)调整容量政策与渐进调度。

## 设计与验收

- 自动额度使用device-local heap当前可用预算，取消1/4、1/2和8 GiB人为折扣；显式小额度仍用于调用者约束和失败验证。额度不是预分配；驱动失败保留已完成帧。
- 去掉渐进总目标射线乘积检查；它跨越许多独立提交，不应限制完成目标。单次dispatch仍分块，保留计数器、数值模型和驱动实际能力检查。
- 去掉desktop几何/纹理数量及源像素字节人为容量封顶；纹理管线按实际数量增长，受设备descriptor上限与实际allocation准入约束。扩容先完整创建候选管线，成功后替换；失败不破坏旧资源。
- 路径视口使用实际面板尺寸，GPU维度和缓冲范围按设备能力检查；CPU暴力参考与固定射线数值接口仍保留各自工作契约。
- Scene media使用更大tile减少CPU资源准备/提交/等待，基准按相同输入比较；必要时对单次工作再拆分，不能以少采样或低分辨率冒充加速。
- 覆盖超旧上限准入、真实低额度拒绝、分块逐值等价、取消/失败/旧帧不可变与默认场景桌面尺寸；required validation与关闭validation的计时分别记录。

## 实际变更

- Renderer默认memory_limit=0自动使用device-local可用预算，本机约22.658 GiB；删除整目标射线总量限制，尺寸改按实际image/storage/address能力检查。小额度预检及分配失败保留已完成帧。
- Editor路径视口使用实际面板尺寸，目标spp采用uint32输入，反弹UI开放到渲染器支持的32；介质与表面统一tile256，设置不降质。
- desktop场景、DiskScene和源glTF导入容量同步扩展；默认离线导入/CPU参考及产物格式保持各自契约。GPU纹理管线按实际数量增长，完整候选创建后替换；真实owner和在途生命周期不变。
- 历史预留分量保存两个uint32射线累计，避免放开目标后计数回绕/float整数失真；不要求shaderInt64，不增加128 bytes/pixel历史大小。合成GPU回归验证跨32位进位。
- 纹理回归覆盖65个发散descriptor和4097×1纹理；非整除257×129介质大/小tile、反向遍历和全部复用组合逐值一致。桌面worker覆盖1600×1200/2560×1440、四模式、外持旧帧、百万目标准入、在途取消和恢复。
- 共享Renderer使独立捕获的GPU预算自动选择；命令schema、guard/effect、原子文件发布不变，实际registry/describe与Python示例验证。三个汇总器按报告额度检查，仍拒绝真实超额。
- 修正本机DK_BUILD_SCRIPTING_LUAU=OFF时捕获验收仍无条件运行Luau示例的问题；CMake仅在该模块关闭时显式跳过Luau子检查，证据记录未运行，不跳过捕获/IPC/数值/Python检查。
- 无依赖版本变化。完整[测量报告](../benchmarks/2026-10-20-media-capacity.md)与[可复算归档](../benchmarks/2026-10-20-media-capacity/README.md)保留18份新旧性能、3份桌面性能及全部中间验证结果。

## 验证结果

- Release `out/verify/20261020-004342-c201ed23`：23项通过，含8个CPU检查；GPU覆盖后来由最终版本复验。
- `out/verify/20261020-005316-8c75d9c8`：最终21项GPU检查通过，0失败/跳过，覆盖progressive、介质与全部ReSTIR调度/状态/取消、显存、纹理、两个桌面worker和真实后台GUI。
- `out/verify/20261020-010055-ade2d1db`：4097纹理扩展、media worker与按设备额度汇总3项复验通过。
- `out/verify/20261020-011600-fa2a6aa0`：3种独立捕获及3个性能汇总器全部通过；commands.list/describe、成功/失败、Python示例及退出保护得到验证。Luau未构建，其示例子检查未运行。
- 合并上述范围，共35个不同检查通过（24 GPU/11 CPU）；未执行全工程、Debug、其他GPU或完整M19.8高spp质量矩阵。
- 中间失败均保存：003637旧8 GiB断言尚未更新；003952移除局部变量误删介质相机引用；004920新计数测试遗漏Graph target依赖；005042 Slang不允许同一声明中引用前一变量；011308未启用Luau却调用--script。修正后相应检查全部复验通过，未以旧程序冒充成功。
- 三轮同输入介质吞吐：Classic 47.52→19.82 ms/spp（2.40×）、initial 69.16→48.51（1.43×）、时间+空间141.68→83.32（1.70×）；提交数降低90%，CPU准备耗时降低85%–89%。32×18颜色/二阶矩与全部来源/统计完全一致。
- GPU区间/wall中位数49.4%→86.1%、45.5%→63.6%、56.8%→92.6%；不是任务管理器或SM占用率。新版tile p95为5.37/8.23/8.63ms，后两项仍略高于历史8ms候选；取消p95均小于6ms。
- 三轮默认相机worker owned峰值2.103 GiB，取消最大12.90ms，全部模式可用；高分辨率复用仍昂贵，未宣称实时全尺寸收敛。冷启动12.58–13.03秒未优化。
- 文档检查通过18份Markdown、725个本地链接及全局元数据/索引/测试入口/JSON；归档哈希和三轮数据离线复算通过，`git diff --check`通过。

## 限制与后续

本次资源容量和介质提交瓶颈已处理。保留硬件/表示范围、有限路径模型和单次dispatch的正确性保护；显存额度不是预分配或驱动保证。
CPU产物/独立捕获格式、算法参数及CPU参考仍有各自合法域。继续优化高分辨率ReSTIR内部GPU成本、冷启动和队列重叠可另立阶段；本次不改变原等误差结论或Classic默认策略。
