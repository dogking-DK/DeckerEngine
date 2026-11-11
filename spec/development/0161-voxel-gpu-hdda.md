---
id: "0161"
created_at: "2026-11-11T18:18:00+08:00"
updated_at: "2026-11-11T18:51:00+08:00"
status: completed
design_refs:
  - ../design/render-voxels.md
  - ../design/graphics-shaders.md
---

# 0161 M23.3 NanoVDB GPU 上传与共享 HDDA

## 目标与依据

实现 [M23.3](../roadmap.md#m23实体体素渲染) 的上传、PNanoVDB/Slang compute 兼容与共享 HDDA；
沿用 [体素契约](../design/render-voxels.md#m233-gpu-实施契约)及 [Slang 门禁](../design/graphics-shaders.md#m233-pnanovdbslang-compute-兼容性)。
GPU 数值门槛在执行前固定，不提前实现 mesh 合成、完整文件资产或 intersection。

## 实际变更

新增 [dk_render_voxels](../../engine/render/voxels/CMakeLists.txt) 与 [公开接口](../../engine/render/voxels/include/dk/render/VoxelGpu.hpp)：

- `VoxelGpuCache` 整份上传不可变 NanoVDB，成功 submit 才发布 generation；cache hit 不提交新工作。
  额度包含存活网格、未完成上传的 staging 及新候选/staging；默认 512 MiB、最多两代。
  失败保持 current/generation，旧代由调用者与在途帧共同保活，完成并释放后回收。
- `VoxelGpuQuery` 提供异步 trace/sample，Graph 导入上传的实际 ledger，连接 transfer→compute→copy→host。
  `VoxelGpuFrame` 保活 execution/grid；renderer/cache 释放或 Memory closing 后仍能等待读回，超时/错误保留调用者输出。
  新工作拒绝 closing/异队列；每次 payload 默认 256 MiB，最多 1 Mi 条射线和每条 1 Mi 个访问区域。
- [PNanoVDB 适配](../../engine/render/voxels/shaders/pnano-adapter.slang)保持厂商头原样，封装只读 uint storage、inout fault、对齐和完整范围检查。
  [共享 HDDA](../../engine/render/voxels/shaders/voxel-hdda.slang)独立于入口，覆盖 1/8/128/4096 层级、空域/tile 跳过、外露表面、边角/擦边、原射线 t 和世界法线。
  非零 inactive 和零 active 的规范化沿用 CPU 快照，大整数材质不转 float。
- 加入默认 OFF 的 `DK_BUILD_RENDER_VOXELS`、`windows-voxels-gpu` preset、[GPU 探针](../../tests/integration/VoxelGpuProbe.cpp)、
  [坏地址 fixture](../../tests/fixtures/shaders/voxel-address-check.slang)和 5 个定向 CTest；同步设计、Roadmap、依赖/构建说明与测试选择表。

依赖方向：PUBLIC sparse Geometry/Device，PRIVATE Graph/Shaders；OpenVDB 只由探针的 CPU fixture 构建器消费。
本阶段按射线批次独立验证，暂不依赖 Scene/RenderData；这是分阶段装配选择，后续 M23.4/5 才接场景合成/资产。

### 固定工具链与布局

Windows x64、Visual Studio 2026/MSVC 19.51.36260、CMake 4.2.1，Debug `/W4 /WX`。
沿用 builtin-baseline `33d78c1ed898a06938f31312167c7abefd229455`、OpenVDB 12.0.1#0 的无 CUDA overlay，未新增或升级三方版本。
NanoVDB/PNanoVDB 32.7.0（ABI major 32）同源；安装 PNanoVDB.h SHA256：
`eed582ab74878225c05bb5e3f08c92ca5a06cfd6075490f873607086f21fe7b9`。

Slang 2026.18、SPIR-V 1.5、row-major、precise floating point。
宏为 `PNANOVDB_HLSL` / `PNANOVDB_BUF_CUSTOM` / HLSL 默认 `PNANOVDB_ADDRESS_32`，
另补 grid_type/常量宏，并令 `pnanovdb_buf_t` 为 `inout DkVoxelBuffer`。
little-endian Int32；set 0 binding 0 是 96-byte uniform，1/2/3 为 grid/ray/result storage，stride 分别 4/48/64，local 64×1×1，无 push constant。
固定头、适配器、共享 HDDA、入口和 precise 选项进入 source_fingerprint；最终本机指纹 `70937b458d91e54b`，产物 264180 bytes，
SPIR-V SHA256 `59abd813294ec3c8079b27397d611db827da070dcf64bcc64d17b35d56621341`，反射 schema 3。
SDK `spirv-val` 2026.4-rc2 使用 `--target-env vulkan1.3` 校验。

## 验证

```powershell
cmake --preset windows-voxels-gpu -DDK_WARNINGS_AS_ERRORS=ON
& ./scripts/verify.ps1 -BuildDir out/build/windows-voxels-gpu -Target @('dk_voxel_gpu_probe','dk_voxel_tests','dk_voxel_sparse_tests','dk_shader_tests') -TestRegex '^dk\.(voxels\.|shaders\.(compute reflects|compilation is repeatable|same process observes|precise floating))' -Reason 'M23.3 PNanoVDB/Slang、SPIR-V、真实 GPU HDDA/离屏/寿命与 CPU oracle、旧 compute 回归' -Parallel 4
& ./scripts/verify.ps1 -BuildDir out/build/windows-voxels-gpu -Target dk_voxel_gpu_probe -TestRegex '^dk\.voxels\.(slang_compute|spirv_validation|gpu_.*_required)$' -Reason 'M23.3 最终源码 LF 和 staging 预算修复后的产物指纹、SPIR-V、真实 GPU 门禁' -Parallel 4
```

最终覆盖 22 个不同检查：13 CPU 体素回归、4 既有 shader 回归、2 编译/格式门禁和 3 required GPU 用例。
原批次 `out/verify/20261111-183105-d6dbfdc7` 的 17 个既有回归通过；该批次 GPU numeric/lifetime 失败及修复见下文，不能把原批次写成全通过。
最终新增 5 项全通过、失败 0、跳过 0，证据为 `out/verify/20261111-184630-42b229fa/{summary.json,results.xml,build.log,tests.log}`。
其间 staging 专项 `out/verify/20261111-184342-8ee674cb` 1/1 通过；未扩大到全工程、Release、Editor 或 RT 回归。

真实设备为 **NVIDIA GeForce RTX 4070 Laptop GPU**，RT 显式关闭，validation required，sync validation 开启；缺 GPU/validation 直接失败，无 skip 路径。
每项 GPU 探针结束 Vulkan errors=0，VMA 分配数和引擎 heap live allocations=0。
每次设备创建有 1 条已知 loader 警告：`VK_LAYER_AMD_switchable_graphics` 的 API 1.3 低于应用 1.4；
只允许既有完整消息精确匹配，其余警告均失败。OpenVDB 空树 fixture 的 GridStats 提示单列为库诊断。

| 验收 | 实际证据 |
| --- | --- |
| Slang/布局/来源 | 两次 fresh compile 的 SPIR-V/指纹相同；include 依赖、4 个 descriptor、uniform/local size、无 RT；SPIR-V validator 通过 |
| 小网格数值 | 空/实/空心/棋盘/随机/单格六类，6144 条射线同时对独立外露面 oracle 与 CPU sparse；1296 个规范化采样对 dense |
| 层级与整数 | 4096/128/8/1 四层读取，背景、非零 inactive/零 active、负坐标、16777217/INT32_MAX 无损 |
| 大 tile/空域 | 8192×4096×4096 双常量 tile 仅 864-byte NanoVDB，访问/跳过各 2，无内部假面；跨度 1048577、两 leaf 仅 613664 bytes，访问 307/跳过 301，不密集化 |
| 边界与变换 | 内部起点/空腔、相邻 tile、跨轴 ties/擦边/近远裁剪、半格、负/非均匀缩放与 shear；CPU double 合成/逆变换一次，t/世界法线对照 |
| 坏地址和工作额度 | 非对齐、末端 uint32/uint64 越界、UINT_MAX、禁止写入在真实 GPU 返回 fault，合法尾部值 42 保留；步数超限返回错误且不覆盖 sentinel 输出 |
| 提交与寿命 | 注入上传/query submit OOM、确定性 timeout；字节/两代额度、未完成 staging、完成后重试、foreign queue、旧帧跨替换、owner 释放、closing 后读回及禁止新工作 |
| 离屏 | 64³、640×480 两投影的每条 GPU 命中对 CPU dense；hit/cell/value/face 精确一致，距离 ≤1e-4×max(1,abs(t))、法线分量 ≤1e-4，无边缘遮罩放宽 |

离屏 CPU 树 305408 bytes，NanoVDB CPU/GPU 各 305088 bytes；单次 query 输入/uniform/output/readback payload 合计 54067296 bytes。
透视命中 168100 像素、正交 245760；最终单次 host submit/wait/read 为 1066.055/1189.272 ms，包含 CPU 打包和等待，**不是纯 GPU 计时或帧性能承诺**。
产物位于 `out/build/windows-voxels-gpu/tests/integration/voxel-artifacts`：SPIR-V/反射 JSON、两幅 PPM。
PPM 是 GPU 命中读回后 CPU 编码的材质诊断图，已逐像素比对；透视 PNG 辅助检查无异常。尚非最终 HDR 图像或独立 render.capture。

### 发现与修复

首次配置因 vcpkg 下载 Python embed 工具的 TLS 验证失败；改用同一官方 URL 下载后由 vcpkg 校验预期摘要，配置成功，未更换依赖来源/版本。
首次 GPU 数值/寿命用例暴露默认 `DBL_MAX` 远端被 host 拒绝；根据已限制的原点/域/方向推得 slab 交点 ≤2.1e14，
将超 float 上限的远端作为无界钳制至 FLT_MAX，并拒绝正近端下溢为零。
随后双常量 tile 用例命中虽正确，但负坐标跨区仅记录一次访问；将层级对齐改为幂二掩码，拆开共享 inout buffer 的嵌套调用，
保持原有访问/跳过数断言并通过全部数值门禁。该证据不单独归因于某个编译器/驱动的 remainder 实现。
复核补齐旧上传 staging 预算，确定性未完成用例验证超额拒绝/当前代保留，以及完成后回收与重试成功。

`scripts/check-spec.ps1 -Path` 限定本次 10 份变更文档通过，校验 1036 个本地链接及元数据/表格/索引/测试入口/JSON manifests；
初次检查发现测试表的 regex 竖线未转义，修正后通过。`git diff --check` 通过。

## 决策与限制

首期 float 索引域/逆变换后原点 ±2^20，非零方向分量 [1e-8,2^20]，32-bit storage 字节地址且受 maxStorageBufferRange 约束；
超范围、数值不推进和地址错误显式报告，不把错误当作 miss。不要求 CUDA、shaderFloat64、RT/BDA。
GPU 仅消费 CPU builder 发布的可信不可变 Int32 快照；尚无任意 NanoVDB 字节解码入口。
payload 预算不包含驱动分配粒度、编译器/OpenVDB scratch 或进程 RSS，完整性能/显存峰值测量留待 M23.7。
只验证了本机 Windows/NVIDIA compute；其他驱动以及 64-bit 地址未验收。
M23.3 完成，下一阶段为 M23.4 mesh/voxel 深度/HDR 合成；正式文件/资产、Editor/capture、M23.8 intersection 和 M23.9 路径积分仍待后续阶段。

## 修改记录

- 2026-11-11T18:18:00+08:00：创建实施记录并固定 GPU 接口、寿命和数值验收门槛。
- 2026-11-11T18:51:00+08:00：完成上传/共享 HDDA、真实 GPU 数值/离屏/寿命与文档门禁，记录修复及当前限制，同步阶段状态。
