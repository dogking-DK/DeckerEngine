---
id: "0115"
created_at: "2026-10-14T16:25:00+08:00"
updated_at: "2026-10-14T16:59:14+08:00"
status: completed
design_refs:
  - ../design/render-pathtracing.md
  - ../design/graphics-raytracing.md
  - ../design/graphics-shaders.md
---

# 0115 M16.7.1 路径追踪纹理与alpha-mask

依据[纹理与mask契约](../design/render-pathtracing.md#m1671-纹理与mask实施契约)，
扩展CPU不可变输入/采样、Device独立非一致索引能力、shader能力反射及GPU固定descriptor数组。
范围保持Lambert，金属粗糙度模型留给M16.7.2。

## 实际变更

- [CPU场景](../../engine/render/pathtracing/src/PathTraceScene.cpp)拥有被引用RGBA8、origin/AssetId、sampler、UV和mask参数；
  按资产/纹理身份去重，固定64张/2048单边/64MiB RGBA8额度，缺失/重复/非法数据和预算失败不发布候选。
  指纹覆盖实际消费的纹理/UV/mask；texture_info保留来源，scene/image报告texture_count和masked_emitter_count。
- [CPU采样](../../engine/render/pathtracing/src/PathTexture.cpp)实现标准sRGB解码、nearest/linear、三种wrap与float alpha比较，
  radiance/visibility共用nearest_path筛选。opaque忽略alpha；mask发光三角形不进入NEE分布，BSDF命中PDF权重为1。
  model_version从1改为2，sampler仍为1；旧版本明确拒绝。同步CPU示例的renderer/model_version及产物测试。
- [Device](../../engine/graphics/device/include/dk/graphics/Device.hpp)独立SampledImageIndexingMode请求，支持/实际启用区分；
  required参与选卡，if_available保持既有排序，disabled不启用；只请求shaderSampledImageArrayNonUniformIndexing。
  [shader产物](../../engine/graphics/shader-types/src/ShaderArtifact.cpp)只新增5301/5307能力，实际SPIR-V提取与元数据互校；
  shader创建前核对逻辑设备启用。dk-shaderc schema v3增量添加requires_sampled_image_indexing字段，CLI参数与Runtime注册不变。
- [GPU renderer](../../engine/render/pathtracing/src/PathTraceRenderer.cpp)按已启用能力编译固定64 image/sampler数组变体，
  所有槽有效绑定，布局复用逐set/per-stage/aggregate limits拒绝。按标准公式预解码为RGBA32F再用硬件过滤，
  上传与AS在同一Graph提交；trace声明所有可能访问的纹理，pending保留image/view/sampler，失败和取消保留旧结果。
  [shader](../../engine/render/pathtracing/shaders/pathtrace.slang)以NonUniformResourceIndex访问不同材质，按重心插值UV，
  主/阴影any-hit统一mask及背面筛选。未启用能力时仍可使用无纹理路径，纹理scene和相关shader明确拒绝。
- 新增[CPU纹理测试](../../tests/unit/PathTextureTests.cpp)、[GPU材质案例](../../tests/integration/PathTextureCases.hpp)、
  [glTF夹具](../../tests/fixtures/assets/pathtrace-mask.gltf)；复用导入/CPU产物再加载到路径追踪，测试只修改自身输出目录副本。
  同步设计/使用指南、命令参考、CMake、测试选择表和Roadmap，无依赖升级或Scene持久化格式修改。

## 验证记录

环境：windows-graphics/RelWithDebInfo，MSVC 14.51、现有固定vcpkg baseline，RTX 4070 Laptop。
GPU必须启用Khronos validation和CTest同步验证，缺失算失败；沿0114的进程级旧AMD层兼容处理，
临时设置DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1并在finally恢复，不修改系统设置。

日志均在out/verify/下对应运行目录，含summary.json/build.log/tests.log/results.xml；以下运行编号日期均20261014。

| 运行 | 范围及结果 |
| --- | --- |
| 163600-8a667ca6 | sandbox无法访问现有vcpkg，重新配置失败；未运行测试，随后使用获准的外部工具链访问 |
| 163616-e9aa174e | 拥有型PathTexture容器误用需默认构造的vector重载，构建失败；改为allocator-only构造 |
| 163701-04dd074a | CPU纹理/错误、Device能力、Shader反射8项通过 |
| 164037-fbf78e3c | 探针缺ResourceFactory完整类型include，构建失败，未运行旧二进制 |
| 164210-e7637591 | 6组GPU：3通过/3失败；发现硬件sRGB误差、阴影夹具混入发光反弹、导入夹具缺scratch；修正见下文 |
| 164443-9e7f7b3f | 5项RT策略通过；GPU纹理精度/寿命/disabled通过，mask夹具背面与资产覆盖不足失败；附带RT required条目未重建probe，不计本次证据 |
| 164623-93e230ce | 修正显式双面和资产mask阈值后，mask/asset两组通过 |
| 164848-79a3f615 | 21项CPU参考（含5纹理）、13项shader、24项device、11组required GPU、1个CPU示例产物，70/70通过、0跳过 |
| 165110-b3a45450 | 重建dk-shaderc并运行真实进程反射/旧产物保护：1/1通过 |

合计76个不同条目最终通过（70+5项RT策略+1个shaderc进程），不累加重复运行。
每个最终required GPU组均errors=0、warnings=0、liveAllocations=0，结束前VMA allocationCount也为0。
dumpbin /dependents检查本次CPU参考测试仅flecs、mimalloc及Windows/MSVC运行库，无Vulkan/Slang DLL。
文档检查通过：本次15份Markdown、729个本地链接，以及元数据、表格、索引、测试入口和JSON清单；
git diff --check通过。未纳入工作区已有的文档体系迁移指南及spec入口改动。

主要复现命令：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_pathtracing_probe,dk_pathtracing_reference_tests,dk_pathtracing_reference_demo,dk_device_tests,dk_shader_tests `
  -TestRegex '^dk\.(pathtracing\.|device\.unit\.|shaders\.)' -Reason 'M16.7.1直接受影响链路'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_raytracing_tests -TestRegex '^dk\.raytracing\.unit\.' -Reason 'RT数组布局与limits'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_shaderc -TestRegex '^dk\.shaderc\.process$' -Reason '非一致索引反射真实进程'
```

| 数值/行为 | 实测 |
| --- | --- |
| sRGB、过滤、UV/wrap | CPU独立解析通过；GPU nearest≤3e-3、linear≤5e-3；全部用例最大绝对误差0.00329803669，出现在linear范围 |
| 固定数组 | 64纹理同批发散及最后一槽通过，65输入/低byte额度拒绝；set、stage、image、sampler及aggregate五类limits不足均拒绝 |
| mask与侧别 | radiance/visibility穿孔、alpha线性、factor=cutoff保留、opaque忽略alpha、单/双面及负缩放前后面通过 |
| mask发光统计 | 独立积分0.143674305；8 seed下16384/65536spp均值0.144503951/0.144035371，N/4N差约0.326%＜0.5%；MIS/BSDF-only均通过2%与5标准误差 |
| mask发光CPU/GPU | N/4N nRMSE约2.00122e-5 / 1.03820e-4，低于2%；未纳入NEE计数写入帧诊断 |
| glTF到GPU | 源glTF→compile_asset→load_cpu_artifact→冻结输入→GPU，9命中/1穿孔，身份与CPU一致；冻结后破坏测试PNG导致新导入失败，旧图仍可读 |
| 失败/恢复 | CPU复制分配失败，纹理upload提交失败，bounded Memory初始/中途scene/frame/read失败及恢复、closing、timeout、pending外部释放/queue.close通过 |

## 偏差与决策

本机RGBA8-sRGB硬件解码在高绿色上得到0.9765625，标准值0.97344529，差0.00311721超过预先冻结nearest门槛。
保留门槛，改为标准sRGB逐texel解码后上传RGBA32F；alpha保持线性。结果与先解码再过滤语义一致，
代价为单场景GPU纹理/上传各最多256MiB，后续M16.8统一显存预算时须包含这些资源及在途旧代。
其余失败为测试夹具或构建错误，均有针对性修正；未通过放宽光照/采样阈值获得验收。

## 遗留问题与下一步

M16.7.1完成，下一项M16.7.2为GGX子集；仍是≤64×64无历史Lambert参考。
没有mipmap/ray differential、blend、金属/粗糙度散射、法线贴图、编辑器/capture、实时性能或Sponza容量承诺。
本次仅上述定向验证；未跑全引擎、Debug/Release矩阵、其他GPU/平台、独立CPU-only重新配置或性能基线。
