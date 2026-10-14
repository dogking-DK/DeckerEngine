---
created_at: "2026-10-14T10:18:00+08:00"
updated_at: "2026-10-14T10:18:00+08:00"
---

# CPU路径追踪参考

[M16.1设计](../design/render-pathtracing.md#m161-cpu参考实施契约)提供小场景双精度Lambert参考，
用于后续GPU管线正确性对照。当前尚无硬件RT、渐进式编辑器或路径追踪capture命令。

## 构建与定向验证

使用现有CPU开发预设，依赖保持固定，不需要Vulkan loader或Slang：

```powershell
cmake --preset windows-dev -DDK_BUILD_RENDER_PATHTRACING_REFERENCE=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-dev -Target @(
  'dk_pathtracing_reference_tests', 'dk_pathtracing_reference_demo',
  'dk_render_data_tests', 'dk_geometry_tests'
) -TestRegex '^dk\.(pathtracing|render\.data|geometry)\.' -Reason 'M16.1 CPU reference and direct dependencies'
```

开关默认OFF；启用后提供RenderData与asset_data，要求Scene/Math/Memory已启用。
测试中的Geometry BVH是独立求交对照，reference库本身不依赖Geometry实现。

## 可复现场景示例

```powershell
./out/build/windows-dev/bin/Debug/dk-pathtracing-reference-demo.exe out/pathtracing-reference 128
```

示例为24×24固定相机的开放盒，包含灰色墙面、红/绿侧壁和顶部面积灯；固定seed=42、
4次最大BSDF散射、RR从第3次开始、MIS、黑色常量环境。参数是输出目录与可选spp（默认128），
通过完整输入/工作预算后才执行。CPU Debug适合数值检查，不用于判断未来GPU速度。

- `reference.ppm`：曝光1、Reinhard加sRGB编码后的预览。
- `reference.json`：曝光前线性RGB、模型/采样版本、场景/资产消费摘要、相机、seed/spp、
  反弹/RR、工作预算与实际射线/求交计数。`cpu_seconds`仅统计CPU积分，`gpu_ms=null`。

每个文件独立原子替换，不承诺两个文件的整体事务。修改spp重新从样本0计算，
相同输入和样本区间可重现；不同spp不是时域重投影或持久历史。
集成测试运行两次8spp示例，对比线性数据/PPM并验证非法输入保留已有输出。

## 库使用与限制

入口为[PathTraceReference.hpp](../../engine/render/pathtracing/include/dk/render/PathTraceReference.hpp)。
先从SceneSnapshot提取RenderScene，以CpuAsset只读指针span构造PathTraceScene，
再用RenderView调用render_path_reference；`estimate_path_reference`用于固定射线的解析/统计实验。
快照复制消费数据，源资产/文档修改不影响旧结果；同一revision下资产改变会改变诊断指纹。

参考材质必须显式设置metallic=0、opaque且无纹理；默认MaterialData的metallic=1会拒绝。
使用世界三角形平面法线，忽略顶点法线/UV；单面背面剔除，双面翻转几何法线。
当前是表面参考，不积分空气或云；场景启用介质时必须显式设置surface_only，报告此范围。

最多64×64、4096个计入预算的三角形；样本区间在[0,65536)，相机为刚体透视针孔、
Vulkan深度0..1、有限near/far。每次同时受2^24射线和2^29候选求交上限约束，
超限报错，不自动降低画质。每样本/反弹检查取消；失败返回错误而不是部分图像。
图像是未曝光的拥有型线性RGB，旧结果可在Memory关闭后读取；新分配要求open heap。

检验误差应读取线性报告；不能用PPM截断/曝光后的像素替代收敛判断。
实测结果与限制见[0109](../development/0109-pathtracing-reference.md)。
