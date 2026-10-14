---
id: "0116"
created_at: "2026-10-14T17:16:09+08:00"
updated_at: "2026-10-14T17:38:04+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/graphics-shaders.md
---

# 0116 M16.7.2 metallic-roughness GGX

依据[GGX实施契约](../design/render-pathtracing.md#m1672-metallic-roughness-ggx实施契约)，
显式区分Lambert与metallic-roughness，先CPU eval/sample/PDF与独立积分，再GPU及定向回归。

## 实际变更

- [CPU BSDF](../../engine/render/pathtracing/src/PathBsdf.cpp)提供独立double eval/sample/PDF，
  各向同性GGX、相关Smith、Schlick、双向Fresnel漫反射衰减与可见法线采样，混合PDF一致用于NEE/MIS和throughput。
  零贡献采样不重抽；黑色介质仍有镜面。alpha=max(roughness²,0.0025)在GPU实现前冻结，不含delta理想镜面。
- [公共接口](../../engine/render/pathtracing/include/dk/render/PathTraceReference.hpp)在create末参数显式选择Lambert或metallic_roughness，
  默认Lambert保持解析参考，GGX接受资产默认metallic=1/roughness=1。冻结标量、模式和来源指纹，图像报告material_model。
  model_version=3、sampler_version=1；旧model明确拒绝，CPU示例报告同步。
- [GPU BSDF](../../engine/render/pathtracing/shaders/path-bsdf.slang)独立float实现；三角数据从8变9个float4，
  主路径和直接光统一使用完整BRDF/PDF，纹理/mask、发光策略、Graph提交、取消与旧帧保护沿既有契约。
- [ShaderCompiler](../../engine/graphics/shaders/src/ShaderCompiler.cpp)增加默认关闭的precise_floating_point请求并纳入指纹，
  路径shader显式在SPIR-V target启用，以保护65536样本补偿求和及二阶矩；默认shader策略/CLI/反射schema不变，无依赖升级。
- 新增[独立积分oracle](../../tests/PathBsdfOracle.hpp)、[CPU测试](../../tests/unit/PathBsdfTests.cpp)、
  [GPU案例](../../tests/integration/PathGgxCases.hpp)及精确编译产物夹具。同步索引、设计、指南、测试入口与Roadmap。

## 验证记录

使用windows-graphics/RelWithDebInfo、MSVC 14.51/19.51、RTX 4070 Laptop、现有固定vcpkg baseline。
GPU要求RT/Khronos validation/sync；临时设置DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1并在finally恢复，沿0114兼容处理。
详细日志位于out/verify/20261014-*，含summary.json/build.log/tests.log/results.xml。

- 171932-a628131e：CPU 3通过/1失败；初始半程Fresnel漫反射混合的掠射白炉反射率1.42775，未满足能量门槛。
  在GPU实现前改为互易的入射/出射Fresnel衰减，未调整验收阈值。
- 172051-b1a24b89：修正后4项CPU全部通过，解析端点、互易性、独立积分、能量、采样/PDF质量与材质冻结成立。
- 172401-fa001825：GPU端点/寿命通过，统计在最低粗糙度失败；172558-9c9f01ad以BSDF-only进一步隔离，
  CPU均值0.699990643，GPU均值0.699538916，与普通float累加误差接近。
- 172821-8a192145：Slang session级精确浮点选项未生效；新增非二进制常量65536项及二阶矩回归仍失败。
  改为SPIR-V target级设置并增加产物NoContraction及来源指纹检查。
- 172947-f2775701：target级设置后shader产物与3组GPU全部通过，原阈值保持。
- 173151-bbb55737：最终55/55通过，0失败/0跳过；25项CPU路径参考、14项shader、15组required GPU及1个CPU示例。
  另保存完整LastTest.log到同一目录，避免CTest XML的长输出截断；15组均errors=0、warnings=0、liveAllocations=0，
  每组结束前也验证VMA allocationCount=0。CPU参考测试dumpbin依赖仅flecs/mimalloc与MSVC/Windows，无Vulkan/Slang。
- 文档检查11份Markdown、734个本地链接、元数据/表格/索引/测试入口与JSON清单通过；git diff --check通过。

最终复现命令（GPU环境变量按上段临时设置/恢复）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_pathtracing_reference_tests,dk_pathtracing_probe,dk_shader_tests,dk_pathtracing_reference_demo `
  -TestRegex '^dk\.(pathtracing\.|shaders\.)' -Reason 'M16.7.2直接受影响链路'
```

| 验收量 | 实测与结果 |
| --- | --- |
| CPU端点/互易性/法线 | metallic=0/0.5/1，roughness=0/0.05/0.2/0.5/1；正入射到NoV=1e-6、旋转法线和无效方向全部通过 |
| 独立积分/白炉/PDF | 加倍网格收敛与反射率≤1.005通过；roughness=1白金属解析1-ln(2)=0.306853，采样0.307040；最低粗糙度白金属0.999996；PDF质量匹配保留的拒绝采样概率 |
| GPU太阳解析端点 | 最大相对误差3.07345223e-7，低于1e-3；包括黑色介质与缺材质默认金属 |
| 非二进制常量 | 65536项Lambert颜色及二阶矩误差≤1e-6；显式precise修复前颜色0.699546933对0.699999988失败，修复后通过 |
| GGX白炉/面积灯 | 8 seed、N=16384/4N=65536；含最低粗糙度掠射，MIS/BSDF-only/RR相对独立积分最大均值差0.8084%，均满足2%与5标准误差 |
| GGX CPU/GPU统计 | 最大nRMSE=8.12065901e-5（0.008121%）；最大N/4N差0.138615%，低于2%/0.5%原门槛 |
| 多次反弹/RR | 四次散射CPU参考0.734190186，GPU 4N=0.734190255；RR从1/3开始均值与无RR一致，CPU/GPU nRMSE最大2.5273e-7 |
| 状态/寿命 | 模式/metallic/roughness指纹、非法输入旧代保护、连续样本、纹理/mask穿孔、上传失败不推进票据、pending释放/timeout/close通过 |

## 偏差与决策

初始glTF半程向量Fresnel混合未通过本项目白炉能量要求，故选用互易、保守的双向Fresnel介质层近似；
该公式在CPU验收后才用于GPU，不宣称完整glTF BRDF。未靠亮点截断或放宽阈值验收。
高样本近常量输入另暴露Slang局部precise不足以保留补偿运算；target级精确模式同时修复既有Lambert累加。
仅路径追踪显式使用该模式，其性能成本尚未测量。

## 遗留问题与下一步

M16.7.2完成，M16.7材质组两项均完成；下一项M16.8为渐进累积与tile/工作/显存额度。
本阶段仍是≤64×64无历史表面参考，不含法线/metallic-roughness贴图、完美镜面delta、透射、
多次微表面散射补偿、编辑器/capture或实时性能承诺。未跑全引擎、Debug/Release矩阵、其他GPU/平台、
独立CPU-only重新配置或性能基线；既有工作区文档体系迁移指南及spec入口改动不纳入本提交。
