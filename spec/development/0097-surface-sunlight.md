---
id: "0097"
created_at: "2026-10-13T09:13:35+08:00"
updated_at: "2026-10-13T09:35:04+08:00"
status: completed
design_refs:
  - ../design/render-pipeline.md
  - ../design/render-atmosphere.md
  - ../design/architecture.md
---

# 0097 M14.4 最小日光接收

## 目标与设计依据

完成[Roadmap](../roadmap.md) M14.4，先细化[管线契约](../design/render-pipeline.md)与[大气边界](../design/render-atmosphere.md)。
显式 Lambert 太阳直射和天空漫射，保留默认 unlit/emissive。双面/镜像约定依据设计中链接的 Khronos glTF 2.0 规范；无上游代码移植或新增三方依赖。
M14.5 天空/空气透视合成和 M14.6 上层集成不在本次范围。

## 实际变更

- ScenePipeline 新增 render_sunlight，要求大气快照环境与 RenderScene 完全匹配。原 render 保持 unlit；disabled 场景退回 unlit。
- 大气启用时 pipeline PRIVATE 链接 render_atmosphere；首次显式调用创建完整 lit 深度/颜色管线后缓存。AtmosphereSky 增加只读锚点与借用 GPU LUT Image 查询。
- sunlight.slang 以 albedo/pi 计算太阳直射与天空漫射，emissive 独立相加，曝光仍由 tone 单次执行。直射逐片段查 T，包含行星遮挡和顶外进入大气光程；Sky View 不含太阳盘，256方向角积分不会重复加入盘能量。
- CPU 使用完整仿射逆转置与正尺度归一化，GPU 先稳健归一化源顶点法线再插值、片段重归一化。缺失/零/抵消法线用面向观察者的几何导数法线。负 determinant 修正正反面，单面在 depth/color 同时剔除，双面翻转 authored 背面法线；alpha-mask 规则一致。
- 世界位置采用相对快照锚点的 km 坐标及独立高度；拒绝奇异变换、地下保守包围盒、超过2H或±1e9m的接收体。天空漫射为锚点局部近似，不是逐点GI。
- Graph 显式导入只读 T/Sky 和逐 draw uniform。整帧提交前完成必要分配/校验；失败不改变场景/大气代次/旧帧，提交保留资源，旧帧可在缓存更新、调用者销毁和域关闭后完成。
- 新增 required GPU 探针及 dk-sunlight-demo：正午/日落/暮光 × unlit/仅直射/含天空漫射，共九张960×360 PPM；球体/非均匀缩放/镜像椭球，固定曝光5，逐文件原子输出。
- 同步设计、[使用指南](../guides/atmosphere.md)、构建入口、测试选择表和 Roadmap。没有命令或场景格式变更。

## 验证记录

环境：Windows x64、windows-graphics Debug、MSVC 14.51、Slang 2026.18、RTX 4070 Laptop GPU、驱动596.49。
Khronos required 与同步 validation 开启。仅进程设置 DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1，测试后恢复原值，隔离本机旧AMD隐式层告警。

共10项不同测试通过：4项CPU管线、4项日光GPU探针、1项球体样例、1项原unlit GPU回归。0跳过、0未解决失败；GPU错误/警告为0，VMA/Memory分配回零。

| 检查 | 有效证据（out/verify/） | 结果 |
| --- | --- | --- |
| 管线参数/clip矩阵/空帧与新增法线逆转置 | 20261013-091659-42436c66 | 4/4；剪切切线正交、镜像及奇异/极小比例 |
| Lambert法线、镜像/剪切、单/双面、缺失/零/抵消/大法线、变化法线插值、纹理/mask/emissive | 20261013-092941-c9cb7197 | normals通过；39个RGB通道比较最大0 byte误差 |
| 逐片段透射、曲率/远原点、低锚点高接收体、行星遮挡、顶外光程、曝光与disabled | 20261013-093131-4e126d8f | direct通过；CPU2048段参考，102个RGB通道最大1 byte误差，完整输出保存full-tests.log |
| 天空漫射 | 20261013-092645-55c48188 | diffuse通过；12组方向/太阳角，36个通道最大1 byte误差 |
| 错配/空快照/奇异/地下/对象/submit/跨队列及跨队列LUT、预算逐步释放/部分失败、关闭、旧帧与在途销毁 | 20261013-092645-55c48188 | failures通过；太阳能量变化的独立结果亦通过，3个通道最大0 byte |
| 九张球体PPM及required GPU/sync | 20261013-092645-55c48188 | demo通过，零错误/警告/泄漏；转换PNG并目视核验九宫格 |
| 原unlit图像、HDR传输、失败与预算/在途寿命 | 20261013-092031-450286be | pipeline_gpu_validation通过，原5种图像与完整图比较保持 |

数值门槛在实现前定为tone后RGB≤2 byte；不把LDR比较写作HDR全图误差证明。
直射测试覆盖1m、1km、100km、150km，以及太阳局部天顶余弦0.8、0.04、-0.1、-0.2；曝光1/3。
1m低太阳角T=(0.357886,0.100637,0.008893)，背日T=0；150km向下穿大气T=(0.467175,0.132599,0.146595)。
另验证1m天空锚点与100km接收体分离、行星/场景整体移到(1e8,2e8,3e8)m后沿曲面移动。
天空参考对同一Sky LUT独立做8192/32768方向角积分，收敛相对差<1%（分母下限0.001）；GPU256方向结果再比较真实绘制像素。
它验证局部漫射积分，不替代M14.3已验收的天空传输，也不宣称完整多阶光照真值。

实际命令均使用 scripts/verify.ps1 -BuildDir out/build/windows-graphics，按上述顺序定向执行：

```powershell
# CPU 4/4
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Target dk_render_pipeline_tests `
  -TestRegex '^dk\.render\.pipeline\.' -Reason 'M14.4 管线接口与法线矩阵初步编译验证'
# 首轮4/5：旧管线/数值通过；失败测试错误假设由下述批次修正。
-Target @('dk_surface_sunlight_probe','dk_render_pipeline_probe')
-TestRegex '^dk\.render\.(sunlight_.*_gpu_validation|pipeline_gpu_validation)$'
# 20261013-092301-7dafd7cf：修正显式等待并补充预算/关闭/异队列LUT，1/1。
-Target dk_surface_sunlight_probe
-TestRegex '^dk\.render\.sunlight_failures_gpu_validation$'
# 20261013-092645-55c48188：4/5；新增镜像缺失法线回归失败，随后修复。
-Target @('dk_surface_sunlight_probe','dk_sunlight_demo')
-TestRegex '^dk\.render\.sunlight_.*_gpu_validation$'
# 最终仅复核修复的法线分支；其它有效结果未覆盖，未重复跑。
-Target dk_surface_sunlight_probe
-TestRegex '^dk\.render\.sunlight_normals_gpu_validation$'
# 新增实际接收位置/顶外穿大气/大世界边界，1/1。
-Target dk_surface_sunlight_probe
-TestRegex '^dk\.render\.sunlight_direct_gpu_validation$'
```

GPU每条verify调用外层用try/finally暂存/恢复上述进程环境变量；详细Target、TestRegex和返回结果见各summary.json。
图片位于 out/build/windows-graphics/test-artifacts/Debug/sunlight；comparison.png按行正午/日落/暮光、按列unlit/直射/天空漫射。

最终文档检查通过12份Markdown、673个本地链接及元数据/表格/索引/测试入口/JSON清单；git diff --check通过。

```powershell
& ./scripts/check-spec.ps1 -Path @(
  '.agents/skills/decker-build-verify/references/test-selection.md','README.md','spec/design/README.md',
  'spec/design/architecture.md','spec/design/render-atmosphere.md','spec/design/render-pipeline.md',
  'spec/development/0097-surface-sunlight.md','spec/development/README.md','spec/guides/atmosphere.md',
  'spec/guides/build.md','spec/guides/render.md','spec/roadmap.md'
)
git diff --check
```

## 偏差与决策

- 首轮失败测试把太阳能量变化的prepare缓存命中当作隐式等待；缓存命中不提交LUT，改为显式等待旧帧，保持生产异步契约。
- 复核发现顶点法线仅按最大分量缩放会改变不同顶点插值权重；先稳健单位化源法线，加入变化/抵消法线回归。
- 镜像缺失法线回归揭示屏幕导数法线不随索引绕序变向，重复按front/determinant翻转错误；fallback固定面向观察者，authored法线单独处理背面，最终回归通过。
- 文档首轮检查发现开发索引表行间多余空行，修正后重新检查。
- 天空漫射固定在快照锚点，逐片段256次Sky采样优先提供可核验基线，未做性能优化。包围盒地下检查保守，可能拒绝真实表面未入地的大跨度接收体。

## 遗留问题与下一步

M14.4范围完成。下一项M14.5实现天空/几何HDR合成、空气透视和受限线性诊断读回。
未运行Release、全引擎、GUI、跨厂商、性能基线或关闭大气开关的独立构建；本次无依赖版本调整。
无镜面/PBR、几何阴影、地表GI、材质unlit扩展标记或自动Editor/capture装配；保留显式整帧unlit入口。
用户原有spec/README.md与spec/guides/ai-documentation-workflow.md保持、不纳入提交。

## 修改记录

- 2026-10-13T09:13:35+08:00：先细化设计并创建记录。
- 2026-10-13T09:33:57+08:00：完成实现、10项不同定向测试和九张样例视觉核验，收尾文档检查。
