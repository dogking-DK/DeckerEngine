---
id: "0111"
created_at: "2026-10-14T11:15:47+08:00"
updated_at: "2026-10-14T11:31:09+08:00"
status: completed
design_refs:
  - ../design/graphics-raytracing.md
  - ../design/graphics-resources.md
  - ../design/architecture.md
---

# 0111 M16.3 拥有型BLAS/TLAS与候选构建

目标：在M16.2 RT设备上完成首版device BUILD、尺寸/输入校验、独立scratch及现有提交域保活。
沿用固定依赖；不接入RT shader/SBT/Graph/编辑器，不提供UPDATE、压缩或裸地址实例接口。

状态提交点：create返回未构建候选；build录制本地状态；submit成功发布generation与访问；
poll/wait/close确认后标completed。旧代由调用者保留，失败不触碰它；pending保留AS、输入、scratch和BLAS引用。

## 实际实现

- [AccelerationStructure.hpp](../../engine/graphics/device/include/dk/graphics/AccelerationStructure.hpp)提供move/share拥有型对象、
  sizes/state/generation和借用原生handle/address；ResourceFactory创建独立BLAS/TLAS候选，CommandBatch::build执行首次BUILD。
- [AccelerationStructure.cpp](../../engine/graphics/device/src/AccelerationStructure.cpp)查询驱动尺寸、创建独立VMA storage和RAII AS，
  将输入保留至构建完成；TLAS复制实例数据并全寿命保留BLAS。scratch每次录制独立分配，对实际BDA做alignment与剩余区间校验。
  storage/scratch/input与AS原生句柄按正确顺序释放，沿用当前Device dispatch与Memory resource。
- [AccelerationPolicy.hpp](../../engine/graphics/device/src/AccelerationPolicy.hpp)校验几何/实例计数和设备上限、float3格式、
  stride/offset/索引宽度/范围、非零地址及溢出、scratch容量、有限可逆3x4变换、24-bit字段/8-bit mask/基本flags。
  Buffer AS专用usage在分配前要求RT启用。索引内容须由调用者保证小于vertex_count；不隐式读回GPU输入。
- [ResourceInternal.hpp](../../engine/graphics/device/src/ResourceInternal.hpp)和Resources.cpp添加独立AS录制/提交账本，
  不混同AS与buffer内容。独立scratch不标记业务初始化；类型化build处理host/upload到输入及BLAS到TLAS依赖。
  成功submit的noexcept提交段才转移保活/发布代；放弃或失败释放预约且候选可重试，旧对象保持可用。
- [纯策略用例](../../tests/unit/AccelerationStructureTests.cpp)与[required GPU探针](../../tests/integration/AccelerationStructureProbe.cpp)
  分别覆盖数值拒绝和真实构建/状态/释放。更新原Buffer策略测试、测试CMake及选择表；不新增生产target、依赖或命令。

## 验证与证据

windows-graphics/RelWithDebInfo，MSVC 14.51和既有固定依赖。日志保留在out/verify各编号目录的
summary.json、build.log、tests.log、results.xml。两个GPU运行均仅在验证进程设置
`DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1=1`并finally恢复（本机旧AMD隐式层规避沿用0110的manifest核验）；
Khronos validation为required，CTest同时设置VK_LAYER_VALIDATE_SYNC和VK_VALIDATION_VALIDATE_SYNC。
未改注册表/持久环境，required探针缺硬件或validation直接失败，不能以77跳过。

| 运行编号（20261014） | 实际范围 | 结果 |
| --- | --- | --- |
| 112427-bbdc2ee6 | 5 AS策略、12资源策略、AS/RT设备/资源3个GPU探针 | 19通过、1失败、0跳过；未知instance flags的0x80原始位未被Hpp按位取反捕获，改用原始整数掩码；三个GPU探针均零诊断 |
| 112719-13d664a3 | 修复后5 AS策略及required AS GPU；补齐uint32索引和pending整条TLAS引用链释放 | 6通过、0失败、0跳过；GPU errors=0、warnings=0、liveAllocations=0 |

复现命令（GPU运行在上述临时环境内执行）：

```powershell
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target @(
  'dk_acceleration_structure_tests','dk_acceleration_structure_probe','dk_graphics_resource_tests','dk_graphics_resource_probe','dk_raytracing_device_probe'
) -TestRegex '^dk\.(acceleration\.|graphics\.unit\.|graphics\.gpu_resources_validation$|device\.rt_required_validation$)' -Reason 'M16.3 AS and submission regression'
& ./scripts/verify.ps1 -BuildDir out/build/windows-graphics -Configuration RelWithDebInfo -Target @(
  'dk_acceleration_structure_tests','dk_acceleration_structure_probe'
) -TestRegex '^dk\.acceleration\.' -Reason 'M16.3 instance flags fix and AS lifetime verification'
```

GPU实测：NVIDIA GeForce RTX 4070 Laptop GPU、driver596.49、Vulkan1.4.329、scratch alignment128。
一个BLAS含3个几何（none/u16/u32索引），第二BLAS使用同批上传的device buffer，TLAS含3个实例和2个不同BLAS；
同批build依赖、跨批新TLAS引用、非零稳定地址、代提交/完成状态、外来提交域/空输入/非法描述拒绝通过。
native创建失败不泄漏；录制放弃可重试、重复BUILD拒绝；提交失败候选不发布且旧TLAS/BLAS账本保持；
timeout保留pending；释放外部BLAS/TLAS/input后队列析构等待并回收；TLAS跨queue寿命继续保活Device/BLAS。
device_lost注入发生在native submit之前，不丢弃真实在途GPU工作；验证lost状态、地址拒绝和关闭释放。
VMA allocationCount及最终Memory liveAllocations归零，零warning/error。

增量共20个不同CTest条目通过：最终6个AS条目加首轮未受修复影响的14个资源/设备条目；不是重新全跑20项。
定向check-spec通过：7份Markdown、601个本地链接、元数据/表格/索引/测试入口及JSON清单；git diff --check通过。
未运行全引擎、配置矩阵、编辑器或CPU参考积分器；变更限定Device模块。

## 限制与下一步

BUILD only，float3三角形和CPU实例描述；无UPDATE/refit/压缩/序列化。AS尺寸由驱动返回，设备上限不是实测大场景规模。
本阶段验证构建和生命周期，未执行射线shader或证明命中结果；M16.4接入Slang RT阶段、AS descriptor、pipeline/SBT/trace后验收。
Graph AS资源/同步声明仍属M16.5。Roadmap已转到M16.4。
