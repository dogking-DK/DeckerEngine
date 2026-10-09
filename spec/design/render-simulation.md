---
module: render-simulation
created_at: "2026-10-09T10:59:13+08:00"
updated_at: "2026-10-09T11:36:11+08:00"
status: accepted
---

# 模拟布片可视化

## 边界和接口

`engine/render/simulation` 提供 `dk::render_simulation`，PUBLIC 依赖 [GPU XPBD](physics-xpbd-gpu.md)，
PRIVATE Shaders。`DK_BUILD_RENDER_SIMULATION` 默认关闭，windows-graphics 开启。
`ClothRenderer` 通过求解器建图扩展追加独立布片绘制与可选图像读回；不访问 Scene/ECS 或 CPU 粒子数组。
初始化 shader/pipeline；每次 render 接收求解器、固定 dt/count、列/行、图像大小和相机矩阵。
拓扑必须是规则网格且 columns*rows 等于求解器粒子数。非网格通用粒子仍可独立 GPU 求解。

## 数据、同步和输出

vertex shader 从 storage buffer 按 SV_VulkanVertexID 程序化读取规则格网两三角形的粒子；无需上传逐帧顶点或索引。
绘制 Pass 声明 Compute 写出的粒子 buffer 为 VertexShader/ShaderStorageRead；Graph 建立 RAW 顺序与屏障链。
位置最后一次写入之后，速度重建先以Compute读位置，再由Vertex读取；同步诊断应验证这两段访问链，而非要求紧邻写/绘制。
颜色与深度图由 Graph 管理，颜色/深度先由独立clear Pass初始化再进行附件读写，使用明确正交 view-projection 矩阵、深度测试、布片 UV 棋盘颜色和网格单元明暗。
返回拥有型 SimulationFrame，包含 GPU 物理帧、RGBA8 图像、尺寸和步骤；color() 供后续 GPU 消费者分享。
图像读回必须显式启用，wait 后 read_rgba8；默认不进行粒子或图像 CPU 往返。
独立 C++ 示例输出 PPM，用于观察初态/下垂/地面接触；不声称已接入编辑器 Play 视口或 Runtime 截图命令。

## 提交与失败

参数/尺寸/矩阵非有限、拓扑不匹配在建图前拒绝，不推进物理状态。
Pass 与求解在同一图内执行，提交失败不发布新物理状态，也不返回有效帧。
输出与旧帧不可变；销毁渲染器或求解器后，在途资源由提交队列保留直到完成。
GPU 实际执行错误和数值发散沿用求解器契约，不承诺跨设备逐像素相同。

## 验证

CPU/GPU 数值对照之后生成初态和 300 拍图像；检查非背景覆盖、帧间像素变化、暂停 count=0 不改变粒子/步数。
实际同步诊断必须包含 Compute/ShaderStorageWrite → Compute/ShaderStorageRead → VertexShader/ShaderStorageRead；启用 Vulkan 同步验证。
非法尺寸/网格/相机、无读回模式、旧帧和在途销毁验证见 [0076](../development/0076-gpu-xpbd.md)。
