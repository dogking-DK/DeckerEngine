---
created_at: "2026-10-14T14:37:00+08:00"
updated_at: "2026-10-14T14:37:00+08:00"
---

# GPU无历史Lambert路径追踪

[返回项目入口](../../README.md)。[M16.6设计](../design/render-pathtracing.md#m166-gpu无历史lambert实施契约)
提供硬件三角形求交、NEE/MIS/RR和拥有型HDR结果，使用与[CPU参考](pathtracing-reference.md)一致的冻结输入与采样模型。
当前为≤64×64正确性参考，历史累积、纹理/PBR、编辑器和capture命令按Roadmap后续阶段接入。

## 构建与验证

```powershell
cmake --preset windows-graphics -DDK_BUILD_RENDER_PATHTRACING=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo `
  -Target dk_pathtracing_probe -TestRegex '^dk\.pathtracing\..*_gpu_required$' `
  -Reason 'GPU Lambert解析/统计/图像/寿命/分配失败验收'
```

开关默认OFF，消费者链接`dk::render_pathtracing`，包含`dk/render/PathTraceRenderer.hpp`。
Scene/Math/Memory和Graphics Device/Graph/Shader须已启用；GPU开关同时提供CPU reference，
单独开启reference不链接GPU。required探针要求真实RT与Khronos validation，同步验证由CTest设置，缺失不按77跳过。
本机旧AMD隐式层的进程级兼容处理及实测见[0114](../development/0114-gpu-pathtracing.md)。

## 场景、提交与读回

使用`PathTraceScene::create`冻结RenderScene与被引用的CpuAsset；此阶段只接收opaque、无纹理、metallic=0材质。
GPU renderer读取冻结的世界三角形，保留完整仿射结果、几何法线、单/双面、发光与面积权重。
浮点转换导致几何退化或无法表示的正面积/光源权重会被拒绝，旧场景保持有效。

以下展示生命周期；`heap/queue/source/view`分别是开放Memory、已启用RT的提交队列、PathTraceScene和匹配RenderView。
为突出调用顺序使用value()，正式调用应检查Result错误。

```cpp
auto renderer = dk::render::PathTraceRenderer::create(
    heap, queue, "engine/render/pathtracing/shaders").value();
auto scene = renderer.prepare_scene(queue, source).value();
dk::render::PathTraceSettings settings;
settings.environment_rgb = {0.1, 0.1, 0.1};
auto frame = renderer.submit_view(queue, scene, view, settings, {0, 256}).value();
// 实际交互应轮询；这里显式等待以取得数值结果。
if (queue.wait(frame.submission()).value()) {
    auto image = frame.read(queue).value().value();
    const auto rgb = image.linear_rgb();
    const auto moments = image.second_moment();
    const auto hits = image.primary_hits();
}
```

同一队列串行操作；无需在场景构建与trace之间CPU等待，Graph记录GPU依赖。
`prepare_scene`发布不可变GPU场景和BUILD票据，空场景不创建AS，其场景票据value为0。
`submit_view`发布frame及提交票据，不代表GPU已完成；`frame.read(queue)`不阻塞等待，成功但optional为空表示仍在途。
完成后read返回独立CPU拥有型结果；读回或分配失败不发布部分图像，额度恢复后可重试同一frame。
关闭Memory后旧image仍可读，新准备/提交/read拒绝。丢弃renderer/scene/frame不会取消已提交GPU工作，queue.close负责完成和回收。

固定射线数值对照使用`submit_rays(queue, scene, rays, settings, samples, first_pixel)`。
射线归一化、坐标和距离单位米，最多4096条；first_pixel指定计数采样的起始像素编号，不能溢出。
所有请求显式给出连续样本区间，不跨调用积累历史或维护completed_spp。失败/取消由调用者保留上一成功scene/image。
stop_token只在CPU阶段、提交前及读取发布前检查，已提交GPU不可硬抢占。

## 线性结果与诊断

- `linear_rgb()`是曝光前线性RGB均值；`second_moment()`为样本RGB平方的均值。无tone、去噪或亮点截断。
- `primary_hits()`记录每像素本区间第一个样本的主射线命中，含距离、重心、世界法线、front_face和Entity/Asset/primitive/triangle来源；miss为空。
- `info()`记录scene/revision/fingerprint、视图、settings、样本区间、fixed_rays/first_pixel和实际trace射线数。
  空场景compute的硬件射线数为0；硬件三角遍历计数不可得，triangle_tests为0而非测得0次遍历。
- NaN/Inf、负辐亮度和非法诊断拒绝整帧。指纹仅作稳定诊断摘要，不是安全校验或缓存相等证明。

图像≤64×64、场景≤4096三角形、样本区间位于[0,65536)、反弹1..32，沿用CPU工作上界预检，超额不自动降质。
太阳可显式选择冻结Scene太阳，环境为常量RGB；空气/云启用时必须显式surface_only，未实现介质积分。
BLAS从世界顶点合并构建，尚无资产BLAS复用、UPDATE/refit、跨队列AS或渐进会话；GPU浮点与CPU双精度不承诺逐位相同。

## 可执行验收与报告

[PathTraceProbe.cpp](../../tests/integration/PathTraceProbe.cpp)提供五组required测试：
解析/几何/跨尺度阴影、8 seed光照统计、完整图像与分段、失败/在途寿命、内存分配失败和恢复。
CPU oracle使用独立双精度暴力求交，面积灯再用独立二维积分校核；参考先验证N/4N收敛再对比GPU。

images用例在构建目录`tests/integration/pathtrace-hdr.json`写入4×4、seed=42、256spp线性数据。
报告的comparison对象单独记录8 seed、4096/16384spp对照协议和nRMSE，不能把这组误差当作256spp图像的参考误差。
真实结果、门槛与未验证范围见[0114](../development/0114-gpu-pathtracing.md)。
