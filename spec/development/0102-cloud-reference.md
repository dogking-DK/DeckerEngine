---
id: "0102"
created_at: "2026-10-13T13:40:00+08:00"
updated_at: "2026-10-13T14:03:00+08:00"
status: completed
design_refs:
  - ../design/render-clouds.md
  - ../design/render-atmosphere.md
---

# 0102 M15.2 无历史云积分参考

## 目标与设计依据

实现[云参考积分契约](../design/render-clouds.md)：球壳区间、Beer–Lambert、太阳散射/自遮蔽、
能量受控低阶多散射近似及CPU/GPU同模型验证。开工已冻结采样/预算/误差阈值。

## 实际变更

- [CPU参考](../../engine/render/clouds/include/dk/render/CloudReference.hpp)：稳定球壳求交、两段区间、
  地面/有限距离裁剪、Beer–Lambert精确步段权重、散射L/透射T及消光加权代表距离。
  [实现](../../engine/render/clouds/src/CloudReference.cpp)复用M15.1密度公式，提前校验并传入稳定球面高度；
  CPU场生成器和已冻结字节摘要保持不变。云太阳射线独立遍历完整球壳，不被相机终点截断。
- [GPU参考](../../engine/render/clouds/include/dk/render/CloudReferenceRenderer.hpp)同步执行完整射线批，
  host输入→Graph compute→readback；[shader](../../engine/render/clouds/shaders/cloud-reference.slang)独立计算太阳遮蔽及空气透射。
  主射线区间来自CPU；纹理坐标原点以double周期归约，再用局部位移采样，避免大世界float相减。
  每次结果独立拥有，不持有帧历史；预算及输入在GPU分配/提交前校验，提交失败可用原纹理重试。
- 光照包含归一HG、同一环境太阳/大气透射、完整云自遮蔽、显式常量天空边界和可关闭的三项低阶再分配。
  模型版本/权重/诊断均匀模式在reference请求中显式给出；无抖动、早停、空区跳跃或半分辨率。
- 新增纯CPU选项`DK_BUILD_RENDER_CLOUD_REFERENCE`和target；GPU clouds公开依赖reference/Device、
  私有依赖Graph/Shaders，两个独立[CPU](../../tests/unit/CloudReferenceTests.cpp)/[GPU](../../tests/integration/CloudReferenceProbe.cpp)验证目标已登记。
- 修复直接大气透射校验误受默认4km云顶限制的问题：API只拥有AtmosphereParameters，合成用于校验的云壳应适配大气厚度。
  新增H=3000m、实际云壳1000–2000m的CPU/GPU回归，不改变Scene对实际云字段的严格校验。
- 同步模块/架构/源码测试索引、构建指南、测试选择表与Roadmap；没有新增依赖、Scene字段或命令。

## 验证记录

均为Windows x64 Debug定向验证，17个不同CTest用例通过，0失败、0跳过：

```powershell
cmake --preset windows-scripting -DDK_BUILD_RENDER_CLOUD_FIELDS=ON -DDK_BUILD_RENDER_CLOUD_REFERENCE=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_cloud_reference_tests','dk_cloud_field_tests') -TestRegex '^dk\.clouds\.(reference |cloud )' -Reason 'M15.2 CPU shell and lighting reference, convergence and density helper regression'
cmake --preset windows-editor
& ./scripts/verify.ps1 -BuildDir out/build/windows-scripting -Target @('dk_cloud_reference_tests','dk_atmosphere_tests') -TestRegex '^dk\.(clouds\.reference |atmosphere\.)' -Reason 'M15.2 reference acceptance and direct atmosphere transmittance thin-shell regression'
$previousLayer = $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1
try {
  $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = '1'
  & ./scripts/verify.ps1 -BuildDir out/build/windows-editor -Target @('dk_cloud_reference_probe') -TestRegex '^dk\.clouds\.reference_gpu_validation$' -Reason 'M15.2 final GPU reference including thin atmosphere direct sunlight regression'
} finally {
  $env:DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1 = $previousLayer
}
```

- 初轮CPU 8/8通过，日志`out/verify/20261013-135344-8e677a8d`，包含3项原云场回归。
- 薄大气修复后CPU 13/13通过，日志`out/verify/20261013-135900-498c9652`，包含6项云参考、6项大气CPU及同前缀的benchmark汇总脚本用例。
- 初轮GPU和最终GPU均1/1通过；最终日志`out/verify/20261013-135900-6f9a77ca`。
  RTX 4070 Laptop，required Khronos validation + sync validation；验证器errors=0、warnings=0，
  队列pending/recording、VMA allocation和Memory live均归零。AMD旧隐式层仅在子进程环境禁用，结束恢复。
  覆盖预算/无效ray/key在提交前拒绝、foreign queue/texture、原生提交故障注入、失败后重试和保活旧纹理代次。
- CPU均匀介质对解析解L/T绝对误差≤1e-10（消光1e-12、1e-5、.001、1；1/32/256步）；
  GPU最大绝对误差4.58591936e-6，低于冻结2e-3。纯吸收、零源、零coverage/density/extinction/disabled满足精确退化。
- 球壳覆盖云下/内/上、离开壳、内/外切线、两段壳、地下拒绝、地面阻挡及有限终点；
  光照覆盖太阳正/逆向、HG角积分归一、self-shadow/MS开关、大气衰减、不同seed/time与往返时间无历史。

[固定真实场夹具](../../tests/fixtures/CloudReferenceCases.hpp)：generator v1/seed1，128² RG8 + 128³/32³ R8，
coverage=.85，time=17.25s，太阳(.6,.8,0)/irradiance(1,.9,.8)，g=.6、albedo=.9、m=.35，sky=(.015,.02,.03)。
四条射线覆盖云下向上、云内斜向、云上向下及云内水平；原点/方向/距离见夹具。
三档分别256/64/64、512/128/128、1024/256/256。RGB误差使用`max(abs(L-Lref))/max(.01,max(Lref))`：

| 指标（四射线最大值） | T绝对差 | 归一RGB差 |
| --- | --- | --- |
| CPU 256→512 | 3.44249018e-4 | 8.87867406e-4 |
| CPU 512→1024 | 1.28220584e-5 | 8.72166881e-5 |
| GPU 256→512 | 3.28958035e-4 | 8.61553270e-4 |
| GPU 512→1024 | 4.31984663e-5 | 2.00141221e-4 |
| GPU/CPU同256步模型 | 1.25027835e-4 | 3.86058481e-4 |

最后两档及同模型GPU/CPU均满足T≤.005、RGB≤.02，未降低开工阈值。
独立记录模型变化：关闭自遮蔽和MS时固定射线CPU归一RGB变化分别约25.0913%与6.3220%；
这是近似模型开关响应，不是相对物理真值的偏差估计，也不计入离散误差。

`check-spec.ps1 -Path`限定本次9个Markdown文件，通过630个本地链接及元数据/表格/索引/测试入口/JSON检查；
`git -c core.safecrlf=false diff --check`通过。未执行全量、Release矩阵、编辑器画面和实时性能测量。

## 偏差与决策

光照配置是显式reference模型请求，不扩Scene/命令协议；持久化交互在M15.6实现。
GPU参考同步返回每射线L/T，M15.3再接ScenePipeline图像合成；不创建历史或编辑器云预设。

## 遗留问题与下一步

M15.2验收完成。下一项M15.3接入云/空气/几何统一合成、深度裁剪和诊断图。
ScenePipeline环境入口仍拒绝enabled云；本阶段每射线参考不代表编辑器已经能显示云。
低阶再分配及常量天空是受控近似，未对完整多散射路径追踪证明物理精确性。

## 修改记录

- 2026-10-13T13:40:00+08:00：冻结参考模型、支持域和验证门槛，创建记录。
- 2026-10-13T14:03:00+08:00：完成CPU/GPU参考、薄大气修复和定向验收，登记数值证据与后续边界。
