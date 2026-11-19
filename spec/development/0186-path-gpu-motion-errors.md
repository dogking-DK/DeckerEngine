---
id: "0186"
created_at: "2026-11-19T15:14:00+08:00"
updated_at: "2026-11-19T18:14:00+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/render-restir.md
---

# 0186 Sponza 运动路径 GPU 错误定位与复测

0185 的 12 次运动尝试中，原生报过一次 sample/history，640 与 192 各报过一次 medium_domain。旧日志只有错误类别，未保存失败像素和采样编号。通过着色器分支、操作数和批次诊断重现，再用相同工作负载测量修复影响。性能、线性画质和异常分别验收。

固定 low-sun（640×360、Classic MIS、种子 101）在绝对样本 36 的自由程循环复现数值域拒绝。原生、640、192 连续转向均抓到阴影透射循环的相同问题。原生像素 1602514、epoch 55 的随机光学距离为 2⁻²⁵，消光上界为 0.000810892845/m，正的物理步长约 0.00003675 m。旧累加的补偿值却达到 0.00024414 m，超过半个 ULP，使 3935.68115 m 倒退到 3935.68091 m。旧残差公式隐含“当前距离的绝对值至少和新步长一样大”，长自由程打破了这个前提。自由程和阴影现在共用按较大操作数优先的 FastTwoSum 残差。

单射线反例的吸收层位于射线下方，所有采样碰撞均为空碰撞，三样本颜色可直接与环境光比较。在冻结的旧着色器上筛得固定像素计数器 3102398418，触发了同一错误。GPU 真实域和工作预算保护仍然生效。

原生分辨率下按固定姿态顺序重放，在第 342 帧（step 341、yaw 94.492°）、epoch 1023、像素 199265 两次复现 sample/history。着色器在空间合并时得到有限正权重和 2.81953947e-38、目标 0.00535597838、支持数 504。归一化商为 1.04450092e-38，低于 FP32 最小正规数 1.17549435e-38；RTX 4070 Laptop 上的计算冲零，原本正确的非空样本一致性检查将其拒绝。没有观察到 epoch 不匹配或接收点损坏。

三个 RTXDI 归一化入口共用下溢处理。常规商沿用 SDK；仅对有限正质量和分母产生的零商使用独立俄罗斯轮盘。落选清空目标与样本并保留 M；幸存时用缩小 256 倍的分母重算，避免直接乘已经冲零的商。既有非法输入/历史检查仍生效。基准清单新增运行时 Slang 源码哈希，避免相同可执行文件在着色器已变时复用旧轮次。

## 定向验证

- 在计时外将失败像素的正质量、目标与支持数作为运行时 FP32 操作数送入同一个 GPU 归一化函数。原 SDK 商冲零，两个固定抽签分别验证了保留时的权重约 2.67392×10⁻³⁶、淘汰时的规范空样本和两种情况下的 M=504。零支持数仍交由一致性检查拒绝。中庭 smoke 实际提交两帧并通过 Vulkan 验证，记录为 `out/benchmarks/sponza-gpu-diagnosis/weight-runtime.json`。试验中只传编译期字面量的版本未通过，正式结果使用运行时操作数。
- Windows Release 构建 editor、editor render benchmark、pathtracing probe 和 pathmedia benchmark。单射线 `air_numeric_gpu_required` 在旧着色器失败，在修复着色器通过。原始日志分别保存在本地 `out/verify/20261119-153819-52c2ae90` 与 `out/verify/20261119-155847-e3b27c6b`。
- 另外六项 GPU 回归全部通过。ReSTIR 直接光、Scene media v2、空间复用调度、介质加速、云和空气失败处理覆盖直接受影响的链路。命令为 `scripts/verify.ps1 -BuildDir out/build/windows-voxel-editor -Configuration Release -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\.(air_failure|restir_direct|restir_medium_v2|restir_spatial_schedule|cloud_failure|medium_acceleration)_gpu_required$'`，日志位于 `out/verify/20261119-180551-dfa01afe`。
- 新增 `--phase replay` 运行独立的 400 姿态回归。用原生 1629×989、每个姿态 3 spp 逐帧推进并校验采样 epoch、云场和 ReSTIR 版本。修复后 400/400 完成，诊断数、残留分配均为零。原低太阳角固定机位在 640×360 完成 128 spp。计时外另行通过带 Vulkan 验证的编辑器运动画质用例。
- 与 0185 相同的 Sponza SDL 转向中，三档各运行三轮，9/9 一次完成。更新率中位数为原生 3.88、640 档 21.28、192 档 67.18 FPS。三个机位九对运动帧的显示像素逐一相同。线性浮点差及各轮原始指标见[修复前后对照与复测命令](../benchmarks/2026-11-19-media-motion-fix.md)。本次覆盖 RTX 4070 Laptop 单机和固定场景，未运行全量套件或其他 GPU。

失败样本的本地原始证据位于 `out/benchmarks/sponza-gpu-diagnosis` 下的 `low-sun.log`、`diag-0-0/stderr.log`、`diag-7-640/stderr.log`、`diag-8-192/stderr.log`、`stress.log` 与 `stress-detail.log`。最后一次 192 诊断发生在已被较新相机请求取代的工作上，诊断本身可见。用于记录操作数的插桩未进入正式测量。修复后可从 `out/benchmarks/sponza-media-v2-motion-fixed` 查看各次编辑器报告、逐图线性回读和 400 姿态重放结果。
